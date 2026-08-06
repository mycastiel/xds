#!/usr/bin/env python3
"""NDS PREAD bandwidth bench (Python), mirrored from nds_bw_bench.c.

VM / stub CMB:
  sudo PYTHONPATH=file_p2p python3 test/python_nds_bw_bench.py \\
    --topology /dev/nvme0n1 --target /mnt/xds/file.bin \\
    --cmb-va 0 --total-size 1G --io-min 32K --io-max 128K --queue-depth 64

Physical NPU HBM (aclrt):
  source /usr/local/Ascend/ascend-toolkit/set_env.sh
  sudo PYTHONPATH=file_p2p python3 test/python_nds_bw_bench.py \\
    --topology /dev/nvme0n1 --target /mnt/data/f.bin \\
    --npu-device 0 --total-size 256M --io-min 128K --io-max 128K --queue-depth 64
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from typing import List, Optional, Tuple

import nds

SECTOR = 512

ACL_MEM_MALLOC_HUGE_FIRST = 0
ACL_ERROR_REPEAT_INITIALIZE = 100002


class AscendHbm:
    """Allocate device HBM via libascendcl.so (dlopen / ctypes)."""

    def __init__(self, device_id: int, size: int, policy: int = ACL_MEM_MALLOC_HUGE_FIRST):
        self.device_id = device_id
        self.size = size
        self.addr = 0
        lib_path = os.environ.get("ASCEND_ACL_LIB", "libascendcl.so")
        self.lib = ctypes.CDLL(lib_path)
        self.lib.aclInit.argtypes = [ctypes.c_char_p]
        self.lib.aclInit.restype = ctypes.c_int
        self.lib.aclFinalize.argtypes = []
        self.lib.aclFinalize.restype = ctypes.c_int
        self.lib.aclrtSetDevice.argtypes = [ctypes.c_int]
        self.lib.aclrtSetDevice.restype = ctypes.c_int
        self.lib.aclrtResetDevice.argtypes = [ctypes.c_int]
        self.lib.aclrtResetDevice.restype = ctypes.c_int
        self.lib.aclrtMalloc.argtypes = [
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        self.lib.aclrtMalloc.restype = ctypes.c_int
        self.lib.aclrtFree.argtypes = [ctypes.c_void_p]
        self.lib.aclrtFree.restype = ctypes.c_int

        ret = self.lib.aclInit(None)
        if ret not in (0, ACL_ERROR_REPEAT_INITIALIZE):
            raise RuntimeError(f"aclInit failed: {ret}")
        ret = self.lib.aclrtSetDevice(device_id)
        if ret != 0:
            raise RuntimeError(f"aclrtSetDevice({device_id}) failed: {ret}")
        ptr = ctypes.c_void_p()
        ret = self.lib.aclrtMalloc(ctypes.byref(ptr), size, policy)
        if ret != 0 or not ptr.value:
            raise RuntimeError(
                f"aclrtMalloc(size={size}, policy={policy}) failed: {ret}"
            )
        self.addr = int(ptr.value)
        print(
            f"hbm_acl: device={device_id} HBM {size} bytes at 0x{self.addr:x} "
            f"(policy={policy})",
            file=sys.stderr,
            flush=True,
        )

    def close(self) -> None:
        if self.addr:
            self.lib.aclrtFree(ctypes.c_void_p(self.addr))
            self.addr = 0
        self.lib.aclrtResetDevice(self.device_id)
        self.lib.aclFinalize()


def parse_size(text: str) -> int:
    text = text.strip()
    if not text:
        raise ValueError("empty size")
    mul = 1
    if text[-1] in "KkMmGg":
        mul = {"K": 1 << 10, "k": 1 << 10, "M": 1 << 20, "m": 1 << 20,
               "G": 1 << 30, "g": 1 << 30}[text[-1]]
        text = text[:-1]
    value = int(text, 0)
    return value * mul


def align_down(value: int, alignment: int) -> int:
    return value & ~(alignment - 1)


def target_size_bytes(fd: int) -> int:
    st = os.fstat(fd)
    import stat as statmod

    if statmod.S_ISBLK(st.st_mode):
        import array
        import fcntl

        buf = array.array("Q", [0])
        # BLKGETSIZE64
        fcntl.ioctl(fd, 0x80081272, buf, True)
        return int(buf[0])
    if statmod.S_ISREG(st.st_mode):
        return st.st_size
    raise RuntimeError("target must be block or regular file")


def pick_io_size(io_min: int, io_max: int, remain: int, rng: List[int]) -> int:
    rng[0] = (rng[0] * 1103515245 + 12345) & 0xFFFFFFFF
    if io_max > remain:
        io_max = align_down(remain, SECTOR)
    if io_max < io_min:
        return align_down(remain, SECTOR)
    span = (io_max - io_min) // SECTOR
    n = rng[0] % (span + 1) if span else 0
    size = io_min + n * SECTOR
    if size > remain:
        size = align_down(remain, SECTOR)
    return size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--cmb-va", type=lambda s: int(s, 0), default=0)
    parser.add_argument("--npu-device", type=int, default=-1,
                        help="physical: aclrtSetDevice + aclrtMalloc HBM")
    parser.add_argument("--acl-policy", type=int, default=ACL_MEM_MALLOC_HUGE_FIRST,
                        help="aclrtMemMallocPolicy (default HUGE_FIRST=0)")
    parser.add_argument("--total-size", type=parse_size, default=parse_size("256M"))
    parser.add_argument("--io-min", type=parse_size, default=parse_size("32K"))
    parser.add_argument("--io-max", type=parse_size, default=parse_size("128K"))
    parser.add_argument("--queue-depth", type=int, default=64)
    parser.add_argument("--passes", type=int, default=1)
    parser.add_argument("--sequential", action="store_true", default=True)
    parser.add_argument("--random", action="store_true")
    parser.add_argument("--no-register-mem", action="store_true")
    args = parser.parse_args()

    random_off = args.random
    register_mem = not args.no_register_mem
    qd = args.queue_depth
    io_min = args.io_min
    io_max = args.io_max
    total_size = args.total_size
    cmb_va = args.cmb_va
    hbm: Optional[AscendHbm] = None

    if qd <= 0 or qd > nds.NDS_IO_MAX_IO_CNT:
        raise SystemExit("invalid --queue-depth")
    if not io_min or not io_max or io_min > io_max or (io_min | io_max) & (SECTOR - 1):
        raise SystemExit("io-min/io-max must be non-zero 512B-aligned")
    if not total_size or total_size & (SECTOR - 1):
        raise SystemExit("total-size must be 512B-aligned")
    if args.passes <= 0:
        raise SystemExit("passes must be > 0")

    topo_fd = os.open(args.topology, os.O_RDONLY | os.O_DIRECT)
    file_fd = os.open(args.target, os.O_RDONLY | os.O_DIRECT)
    try:
        target_bytes = align_down(target_size_bytes(file_fd), SECTOR)
        if target_bytes < io_min:
            raise SystemExit("target too small")
        goal = total_size * args.passes
        buf_span = qd * io_max
        if args.npu_device >= 0:
            hbm = AscendHbm(args.npu_device, buf_span, args.acl_policy)
            cmb_va = hbm.addr

        ret = nds.init([topo_fd])
        if ret:
            raise RuntimeError(f"nds.init returned {ret}")
        try:
            if register_mem:
                ret = nds.register_mem(cmb_va, buf_span)
                if ret:
                    raise RuntimeError(f"register_mem returned {ret}")
            ctx = nds.io_new_ctx(qd)
            if isinstance(ctx, int):
                raise RuntimeError(f"io_new_ctx returned {ctx}")
            try:
                slots = [
                    {
                        "buf_addr": cmb_va + i * io_max,
                        "length": 0,
                        "busy": False,
                    }
                    for i in range(qd)
                ]
                print(
                    f"topology={args.topology} target={args.target} "
                    f"cmb_va=0x{cmb_va:x} npu_device={args.npu_device}"
                )
                print(
                    f"target_size={target_bytes} ({target_bytes / (1024 * 1024):.2f} MiB) "
                    f"total_size={total_size} passes={args.passes} goal={goal} "
                    f"({goal / (1024 * 1024):.2f} MiB)"
                )
                print(
                    f"io_size={io_min}..{io_max} queue_depth={qd} "
                    f"register_mem={int(register_mem)} "
                    f"pattern={'random' if random_off else 'sequential'}"
                )
                sys.stdout.flush()

                completed = 0
                issued = 0
                inflight = 0
                ios = 0
                next_off = 0
                rng = [1]
                flags = nds.NDS_IO_F_REGISTERED_MEM if register_mem else 0
                t0 = time.perf_counter()

                while completed < goal:
                    batch = []
                    batch_slots: List[int] = []
                    while inflight < qd and issued < goal:
                        slot = next((i for i, s in enumerate(slots) if not s["busy"]), None)
                        if slot is None:
                            break
                        remain = goal - issued
                        length = pick_io_size(io_min, io_max, remain, rng)
                        if not length:
                            break
                        if random_off:
                            max_off = align_down(target_bytes - length, SECTOR)
                            rng[0] = (rng[0] * 1103515245 + 12345) & 0xFFFFFFFF
                            off = ((rng[0] * SECTOR) % (max_off + SECTOR)) if max_off else 0
                            off = align_down(off, SECTOR)
                        else:
                            if next_off + length > target_bytes:
                                next_off = 0
                            off = next_off
                            next_off += length
                            if next_off >= target_bytes:
                                next_off = 0
                        slots[slot]["length"] = length
                        slots[slot]["busy"] = True
                        batch.append(
                            (
                                nds.NDS_IO_OP_PREAD,
                                file_fd,
                                off,
                                [(slots[slot]["buf_addr"], length)],
                                flags,
                                0,
                                slot,
                            )
                        )
                        batch_slots.append(slot)
                        inflight += 1
                        issued += length

                    if batch:
                        n = nds.io_submit(ctx, batch)
                        if n != len(batch):
                            start = 0 if isinstance(n, int) and n < 0 else max(n, 0)
                            for k in range(start, len(batch)):
                                slots[batch_slots[k]]["busy"] = False
                                inflight -= 1
                                issued -= slots[batch_slots[k]]["length"]
                            raise RuntimeError(f"io_submit returned {n}")

                    if inflight:
                        events = nds.io_getevents(ctx, 1, inflight)
                        if isinstance(events, int):
                            raise RuntimeError(f"io_getevents returned {events}")
                        for user_data, res in events:
                            if res != 0:
                                raise RuntimeError(f"I/O failed res={res}")
                            slot = int(user_data)
                            if not slots[slot]["busy"]:
                                raise RuntimeError(f"unexpected slot {slot}")
                            completed += slots[slot]["length"]
                            slots[slot]["busy"] = False
                            inflight -= 1
                            ios += 1

                sec = time.perf_counter() - t0
                mib_s = (completed / (1024 * 1024)) / sec if sec else 0.0
                iops = ios / sec if sec else 0.0
                print(
                    f"RESULT pattern={'random' if random_off else 'sequential'} "
                    f"register_mem={int(register_mem)} io_min={io_min} io_max={io_max} "
                    f"qd={qd} bytes={completed} ios={ios} sec={sec:.6f} "
                    f"mib_s={mib_s:.2f} iops={iops:.0f}"
                )
            finally:
                destroy = nds.io_destroy_ctx(ctx)
                if destroy:
                    raise RuntimeError(f"io_destroy_ctx returned {destroy}")
            if register_mem:
                unreg = nds.unregister_mem(cmb_va)
                if unreg:
                    raise RuntimeError(f"unregister_mem returned {unreg}")
        finally:
            exit_rc = nds.exit()
            if exit_rc:
                raise RuntimeError(f"nds.exit returned {exit_rc}")
    finally:
        os.close(file_fd)
        os.close(topo_fd)
        if hbm is not None:
            hbm.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"python_nds_bw_bench: {error}", file=sys.stderr)
        sys.exit(1)
