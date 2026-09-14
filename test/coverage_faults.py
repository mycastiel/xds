#!/usr/bin/env python3
"""Finite, task/stack-filtered allocation faults plus one namespace read fault.
Requires root, an idle disposable NVMe namespace, CMB stub and p2p_dev loaded.
Build: cc -D_GNU_SOURCE -O2 -Wall -Wextra -Werror coverage_errors.c -o coverage_errors
Usage: sudo python3 coverage_faults.py ./coverage_errors /dev/nvme0n1
"""
import os
from pathlib import Path
import subprocess
import sys

binary, device = sys.argv[1:]
debug = Path('/sys/kernel/debug')
addresses = [int(s.split()[0], 16) for s in Path('/proc/kallsyms').read_text().splitlines()
             if s.endswith('[p2p_dev]') and s.split()[1].lower() == 't']
if not addresses or not min(addresses):
    sys.exit('Cannot read p2p_dev text addresses')


def run(mode, inject=False):
    env = dict(os.environ)
    env.pop('XDS_INJECT', None)
    if inject:
        env['XDS_INJECT'] = '1'
    p = subprocess.run([binary, device, mode], env=env, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
    print(p.stdout, end='', flush=True)
    if p.returncode:
        raise RuntimeError(f'{mode}: exit {p.returncode}')
    return int(p.stdout.rsplit('RESULT ', 1)[1].split()[0])


run('easy')
for kind in ('failslab', 'fail_page_alloc', 'nvme'):
    directory = debug / (Path(device).name + '/fault_inject' if kind == 'nvme' else kind)
    if not directory.exists():
        print(f'SKIP {kind}: kernel interface unavailable')
        continue
    settings = dict(probability=0, interval=1, times=1, space=0, verbose=2,
                    **{'task-filter': 'Y', 'require-start': min(addresses),
                       'require-end': max(addresses) + 1, 'stacktrace-depth': 32})
    if kind == 'nvme':
        settings.update({'task-filter': 'N', 'require-start': 0, 'require-end': (1 << 64) - 1,
                         'status': 6, 'dont_retry': 'Y'})
    else:
        settings['ignore-gfp-wait'] = 'N'
        settings.update({'min-order': 0} if kind == 'fail_page_alloc' else {'cache-filter': 'N'})
    saved = {k: (directory / k).read_text().strip() for k in settings}
    if int(saved['probability']):
        raise RuntimeError(f'{kind} already active; refusing to replace another test')
    def write(k, value):
        (directory / k).write_text(str(value))
    hits = 0
    try:
        for k, v in settings.items():
            write(k, v)
        for mode in (('io',) if kind == 'nvme' else ('open', 'register', 'topo', 'io', 'rio')):
            for interval in range(1, 2 if kind == 'nvme' else 13):
                write('interval', interval)
                write('times', 1)
                write('probability', 100)
                try:
                    result = run(mode, inject=kind != 'nvme')
                    consumed = int((directory / 'times').read_text()) == 0
                finally:
                    write('probability', 0)
                print(f'FAULT {kind} {mode} interval={interval} consumed={consumed} result={result}', flush=True)
                if result and not consumed:
                    raise RuntimeError('Error without injected fault')
                if kind == 'nvme' and (not consumed or result != -5):
                    raise RuntimeError('NVMe completion failure was not effective')
                hits += consumed and result != 0
                if run(mode) != 0:
                    raise RuntimeError('Recovery failed')
        print(f'{kind}: {hits} effective error-path injections', flush=True)
    finally:
        write('probability', 0)
        for k, v in saved.items():
            if k != 'probability':
                write(k, v)
        write('probability', saved['probability'])
run('easy')
