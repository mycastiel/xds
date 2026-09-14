# Short coverage-error tests

`coverage_errors.c` bypasses library validation to exercise driver error paths.
`coverage_faults.py` adds 12 finite allocation-failure positions for open,
registration, topology, transient I/O and registered I/O, followed by one NVMe
internal-error completion. Every injection is followed by a successful retry.
No production driver hooks are added.

`basic_test.sh` runs the `easy` and `range` modes on every invocation, so
`run_all_tests_in_vm.sh` and the dual-kernel matrix include them automatically.
Their per-case results and kernel logs are saved as `coverage-errors-*.c.rejection`
artifacts and included in the regular report. These modes require neither gcov
nor fault injection; `coverage_faults.py` remains a separate optional run.

Run as root on an idle disposable VM with the CMB stub and p2p_dev loaded.
The target must be an unmounted, direct NVMe namespace. All test I/O is a
512-byte read; the stub must map a valid CMB buffer. Allocation injection is
filtered to the test task and p2p_dev stack frames. NVMe injection is namespace
scoped because completion may run outside the submitting task. The script
restores changed debugfs settings in `finally`; unsupported interfaces are
reported as SKIP. Consumed faults with successful operations are distinguished
from effective error-path injections (allocation fallback can succeed).

```sh
make -C test coverage_errors
sudo python3 test/coverage_faults.py ./test/coverage_errors /dev/nvme0n1
```

Kernel requirements: DEBUG_FS, FAULT_INJECTION, FAULT_INJECTION_DEBUG_FS,
FAULT_INJECTION_STACKTRACE_FILTER, FAILSLAB and FAIL_PAGE_ALLOC. Coverage also
requires GCOV_KERNEL and GCOV_PROFILE for dev.o, topo.o, debugfs.o and mem.o.
KASAN is optional. NVMe status 6 is INTERNAL_ERROR, producing EIO; status 1
(INVALID_OPCODE) produces EOPNOTSUPP on the tested kernel.

The small linear-map test additionally covers submission rejection, error
propagation through drain, and closing with an unconsumed failed event:

```sh
sudo dmsetup create xds-coverage --table '0 128 linear /dev/nvme0n1 0'
sudo env XDS_COMPONENT=/dev/nvme0n1 ./test/coverage_errors /dev/mapper/xds-coverage range
sudo dmsetup remove --retry xds-coverage
```

Topology release is deferred; `--retry` allows it to finish before removing the
mapping. To check the absent-provider load failure, unload both modules and
verify that loading p2p_dev fails with ENODEV. Collect gcov after module unload,
before replacing the matching .gcno files; preserve/reset counters between runs.

Measured on release_1 `8bd8c1f`, Linux 6.6.0-xds-nokasan-gcov: 47 effective slab
failures, one NVMe EIO, all recovery checks passed. Page-allocation injection
was ineffective for these small, warmed allocations. Combined with the original
full suite, line coverage rose from 1321/1540 (85.78%) to 1402/1540 (91.04%),
and function coverage from 99/100 to 100/100. This supplement does not rerun the
full stress matrix. The detailed classification, HTML and execution logs are in
`test-results/release_1-coverage-errors-20260911/`.
