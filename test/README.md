# XDS prepared-VM tests

## Host-only topology regression

`basic_test.sh` runs `topo_devt_test` after building the test binaries.
The build itself only compiles it. To run this regression alone without
hardware or a loaded driver, use `./test/topo_devt_test` after building.
It mocks sysfs and ioctls to check `dev_t` discovery without target `/dev`
nodes, namespace and partition sizes in sectors, NDS regular-file and
block-device registration/deletion, the device-name `add_topo` and `del_topo`
wrappers, internal deletion without sysfs lookups, and errors.

## VM tests

`basic_test.sh` is a destructive end-to-end test for the XDS direct read/write
path. It builds the kernel modules and both userspace APIs, uses a QEMU NVMe
Controller Memory Buffer as the read destination and write source, verifies
reads by comparing kernel and userspace CRC32 values, and verifies writes
through independent direct-I/O block-device read-back.

## Prepared VM contract

The VM must have one QEMU NVMe controller with:

- a 128 MiB CMB at offset zero in BAR2;
- two 64 MiB namespaces attached to that controller;
- distinct namespace identifiers, normally 1 and 2; and
- an operating system and matching kernel build tree that contain this XDS
  checkout.

QEMU documents multiple `nvme-ns` devices, explicit `nsid` values, and the
`cmb_size_mb` controller option here:
<https://www.qemu.org/docs/master/system/devices/nvme.html>.

The controller can be constructed along these lines as part of the VM command
line (the rest of the VM options are environment-specific):

```text
-device nvme,id=xds-nvme,serial=xds-test,cmb_size_mb=128
-drive file=xds-ns1.img,if=none,format=raw,id=xds-ns1
-device nvme-ns,drive=xds-ns1,bus=xds-nvme,nsid=1
-drive file=xds-ns2.img,if=none,format=raw,id=xds-ns2
-device nvme-ns,drive=xds-ns2,bus=xds-nvme,nsid=2
```

Create both namespace images at exactly 64 MiB. The NVMe driver must be
booted or loaded with `nvme.use_cmb_sqes=0`; the test refuses to use the CMB
when the driver can allocate I/O submission queues from it.

The VM needs a compiler, matching kernel build files, Python development
headers and setuptools, device-mapper userspace tools (`dmsetup`), mdadm,
e2fsprogs, util-linux, and udev tools.

## Running

The two variables are mandatory. There is intentionally no additional
destructive confirmation because the devices are required to belong to a
disposable VM:

```sh
sudo XDS_DEV_1=/dev/nvme0n1 \
     XDS_DEV_2=/dev/nvme0n2 \
     KSRC=/lib/modules/$(uname -r)/build \
     ./test/basic_test.sh
```

The script rejects mounted devices, active holders, input paths that are
partitions, incorrect sizes, different controllers, equal NSIDs, an unexpected
BAR2 size, or active CMB SQ usage. It then destroys all existing data on both
namespaces.

Set `XDS_TEST_WORKDIR` to choose the parent for a unique runtime results
directory. Set
`XDS_TEST_KEEP_WORKDIR=1` to retain logs and generated manifests after a
successful run. Failure artifacts are always retained.

## Basic coverage profiles

`XDS_BASIC_PROFILE=quick` is the default. `XDS_BASIC_PROFILE=full` restores
all of the previous combinations and the CRC-enabled live CQ smoke. The
remote, in-VM, and dual-kernel runners forward this variable to the basic
suite. Both kernels in a report must use the same profile.

| Coverage | Quick default | Full |
| --- | --- | --- |
| Raw topologies | All nine, C and NDS C | All nine, all four APIs |
| Raw Python APIs | Direct namespace 1 and RAID0 | Every topology |
| ext4 block sizes | 1/2/4 KiB on direct NVMe; 4 KiB elsewhere | 1/2/4 KiB everywhere |
| Filesystem API/mode combinations | All four APIs and single/queued/threaded on direct 4 KiB; C and NDS C single elsewhere | Every combination |
| Write/read-back pipelines | C on every topology; Python on direct namespace 1 and RAID0 | C and Python everywhere |
| Registered memory and rejection checks | Unchanged | Unchanged |
| Timed live CQ smoke | Separate opt-in live suite | Included |

The quick profile retains each selected manifest without shrinking its I/O
sizes or dropping boundary/error cases. Fixed CQ manifest/completion checks
remain in `cq_check_test.sh`, included by both matrix runners. Standalone
basic testing does not run that separate suite.

The recorded full basic run took 33m 12s. Quick reduces the main read matrix
from 252 manifests / 1,320 operations to 48 / 304, and read data from
1,624,387,584 to 411,345,920 bytes. Registered-memory and error checks are
additional to those counts. Removed outputs contributed about 17m 29s of
kernel-log timestamp spans, which exclude setup and portions of execution;
the measured quick-profile runtime is recorded below. Other topology/API/block-size
interactions receive less routine coverage and remain available in full.

The quick profile subsequently passed on the same non-KASAN GCOV VM in
**9m 06s**, versus **33m 12s** for full (72.6% less time). The run recorded
304 main cases plus 16 registered-memory cases, 306 CRC checks, and no
serious signatures in 97 retained kernel logs. It includes build, setup and
cleanup, but excludes source sync and artifact download. Evidence is in
`test-results/basic-quick-9574eb0-20260915T134424Z/` in the validation checkout.
Separate stress and bounded CQ suites were not part of this timing.

```sh
sudo env XDS_DEV_1=/dev/nvme0n1 XDS_DEV_2=/dev/nvme0n2 \
  XDS_BASIC_PROFILE=full ./test/basic_test.sh
```

The topology sequence is:

1. Direct block-device reads and writes on both namespaces, followed by ext4
   read coverage with 1K, 2K, and 4K blocks on the first namespace.
2. A partition on the first NVMe namespace, tested for raw reads and writes
   and with 4 KiB ext4 blocks (all three sizes in full), then removed before
   stacked-device testing.
3. A 126 MiB dm-linear device made from the 63 MiB usable region of each
   namespace. Each segment starts at a 1 MiB underlying-device offset to match
   the real LVM data layout. It is tested for raw reads and writes and with
   4 KiB ext4 blocks (all sizes in full), and is created and removed with
   `dmsetup`.
4. The same dm-linear coverage with equal-sized partitions starting at
   different namespace sectors as its components.
5. A two-member, 64 KiB-chunk MD RAID0, tested for raw reads and writes and
   with 4 KiB ext4 blocks (all sizes in full).
6. A two-member RAID0 created with the namespace arguments reversed, verifying
   topology discovery orders components by MD slot rather than sysfs directory
   enumeration order.
7. The same RAID0 coverage with equal-sized partitions starting at different
   namespace sectors as its members. Registration checks accept unequal
   member offsets, including starts not aligned to a chunk, and reject unequal
   sizes.
8. Two partitions on the same NVMe namespace, with the same RAID0 registration
   checks and raw read/write coverage. Component sizes remain equal and
   chunk-aligned; data starts need not align to the chunk size.

Every filesystem phase runs individual boundary cases. Direct 4 KiB ext4
also runs four queued reads and four simultaneous same-fd submissions for
all APIs; full repeats these modes everywhere. The C and Python APIs use
separate CMB ranges.

For each selected raw API/topology pair, the write pipeline first places a
deterministic pattern into a seed range with `O_DIRECT`, reads it into three
HBM IOVs through XDS, drains and validates the read-side HBM CRC, then writes
the unchanged IOVs to a distinct raw range. An `O_DIRECT` host read verifies
the entire payload plus known guards immediately before and after it. The
transfer crosses CMB-page, queue-limit, and topology boundaries. Write-only
kernel logs are checked to ensure that read-only CRC and PA-content diagnostics
were not emitted.

The first namespace also runs the write pipeline with registered memory for
both APIs. Separate rejection tests cover invalid operations, registration
reserved fields, unknown flags, read-only file/device descriptors, shared
write extents, logical gaps, insufficient coverage, and device capacity.

Regular-file writes run on direct-NVMe ext4 filesystems with 1/2/4 KiB
blocks, using the independent second namespace as raw seed/scratch storage.
`file_raw_write_test` checks empty-file growth, unwritten readback,
fragmented mappings, nonzero overwrites with guards, and registered memory.
It verifies payload through independent FIEMAP-based block-device reads
and poisoned XDS readback. It also checks that unwritten metadata remains
unwritten. Ordinary file reads are not the oracle for these raw writes.
This phase never uses a mounted DM/MD component as scratch storage.

The rejection tests temporarily mount ext4 on the second namespace for
their regular-file fixtures. Only the first namespace is registered in
the UAPI test context, so valid file extents stop at topology lookup.
This avoids relying on the checkout filesystem to expose a block device.

`prepare_extents_test` runs locally without XDS devices and covers real
allocation plus controlled FIEMAP errors, flags, and coverage.
`nds_file_prepare_test` checks batch preparation side effects using real
files and mocked submission. Both are built by `make -C test` and run
before the VM data-transfer phases.

The direct first-namespace phase also runs the C and Python registered-memory
tests. They register one sector-aligned but device-page-unaligned CMB range,
reuse its cached PA list across multi-IOV and queued reads, and unregister
after calling `drain_io()`. The C test also races several submitting threads
with unregister and drains accepted contexts before waiting for unregister.
Coverage includes invalid flags and bounds, zero and stale handles, globally
distinct handles, simultaneous use of multiple registered ranges,
same-process reads through another p2p fd, cross-process read and unregister
rejection, owner-fd unregister enforcement, final-fd cleanup, and stub counters
proving PA-list reuse.

A separate deterministic registered-memory CRC32 phase runs after those API
and lifetime tests. Its C and Python runners each register the destination
envelope from a static manifest once, queue four non-overlapping reads through
the same handle on a separate I/O fd, drain them, and unregister the range
through the registration fd. The cases use distinct file offsets and cover
small, offset, device-page-crossing, and larger reads. Their isolated kernel
logs are checked against userspace CRC32 results, while stub counters verify
one PA-list get and put per runner. The unregister race is not part of the
CRC32 phase.

## Dynamic-VA stress test

`stress_test.sh` is a separate destructive entry point for concurrent reads
with runtime-allocated CMB virtual addresses. It uses the same prepared-VM
contract and mandatory device variables as `basic_test.sh`:

```sh
sudo XDS_DEV_1=/dev/nvme0n1 \
     XDS_DEV_2=/dev/nvme0n2 \
     XDS_STRESS_MODE=raid0 \
     ./test/stress_test.sh
```

`XDS_STRESS_MODE` selects the ext4 backing device and defaults to `raid0`:

- `raid0` creates the two-namespace MD RAID0 used by the basic test.
- `dm` creates the two-segment dm-linear device with a 1 MiB data offset on
  each namespace.
- `nvme` uses `XDS_DEV_1` directly.

The test creates 16 files with seeded random, 1 KiB-aligned sizes between
4 KiB and 4 MiB. Their combined size is greater than 4 MiB and capped at
48 MiB so the workload fits the direct 64 MiB namespace. Each of 16 worker
threads owns a different `/dev/p2p_device` fd. On every iteration it
uses a seeded random 1 KiB-aligned file offset and read length, dynamically
reserves the required CMB VA in 4 KiB units, and participates in a concurrent
read. Live allocations must be non-overlapping while at least two files share
a 2 MiB physical CMB page.

The file_p2p and NDS C and Python APIs run normal-memory and registered-memory
variants against the same generated workload. A registered run pins the whole CMB once
through a dedicated registration fd before starting the worker threads. Each
worker issues I/O through its own distinct fd, and the run verifies one PA-list
get and put. Each variant checks its runtime allocation manifest,
kernel/userspace CRC32 results, and debugfs failure/inflight counters. Set
`XDS_STRESS_ITERATIONS` to change the default 16 iterations and
`XDS_STRESS_SEED` to replay another workload; the default seed is `0x584453`.
The common `XDS_TEST_WORKDIR`,
`XDS_TEST_KEEP_WORKDIR`, `XDS_TEST_BUILD_JOBS`, and `KSRC` controls apply to
both entry points. Shared prepared-VM and storage helpers live in `common.sh`.

The default stress suite runs only the dynamic-VA workload. Fixed CQ checks
are provided by `cq_check_test.sh`; the larger CRC-checked repetitions have
been removed from `stress_test.sh`. The full basic profile retains live CQ
smoke; quick omits that timed CRC-enabled workload.
For sustained submit/getevents pressure followed by drain, run
`cq_race_test.sh` explicitly. It retains both NDS frontends and both memory
modes, with 32 workers, 30 seconds before drain and three rounds per
combination by default.

## Bounded CQ manifest and completion checks

`cq_check_test.sh` runs `cq-race` and `cq-race-drain` without CRC, using the
normal module (`build_all normal`). It runs both NDS C/Python APIs and both
normal/registered memory modes. Each of the eight variants has 8 workers and
16 iterations: 128 operations per variant, 1,024 operations total. There is
no timed repetition. This replaces the fixed normal-memory CQ smoke cases
previously in `basic_test.sh` and adds registered-memory coverage.

The existing API runners reject duplicate/invalid events and incomplete
submissions. Without drain, every submitted operation must be returned by
getevents (`completed == submitted == 128`). Drain may consume completions,
so drain mode permits fewer getevents returns. Both modes emit a full result
manifest checked by `check_stress.py` for exact case IDs, counts, file ranges,
VA alignment/bounds, non-overlap and shared backing pages. Registered runs
also require exactly one PA-list get/put; all runs check failure/inflight
counters. CRC checking is omitted.

Use the same disposable-device and `KSRC` settings as the other VM tests:

```sh
sudo env XDS_DEV_1=/dev/nvme0n1 XDS_DEV_2=/dev/nvme0n2 \
  XDS_STRESS_MODE=raid0 ./test/cq_check_test.sh
```

`XDS_STRESS_MODE` defaults to RAID0 and also accepts `dm` or `nvme` for
standalone runs. `XDS_CQ_CHECK_SEED` changes the seed (default `0x584453`).
Both default matrix runners execute RAID0 only. Reports require all eight
CQ outputs, all 128 case IDs per output, a successful manifest-check marker,
and valid completion summaries; no CRC markers are required for CQ checks.

## Running the regression matrix inside the VM

`run_all_tests_in_vm.sh` runs the basic suite, RAID0 dynamic-VA stress,
and bounded RAID0 CQ checks without SSH or rebooting the VM. The default matrix follows the recorded
[coverage and timing analysis](stress-trimming.md). Basic tests retain dm and
NVMe functional coverage; their repeated stress suites can still be run with
`XDS_STRESS_MODE=dm|nvme ./test/stress_test.sh`.

Set `XDS_RUN_CQ_RACE=1` to also run the long live CQ-race suite on all three
topologies. This optional suite requests 18 minutes of overlap per kernel,
plus setup and teardown; the default run keeps bounded CQ accounting checks.
The strict report requires all eight RAID0 dynamic-VA variants and all eight
bounded CQ variants. It also accepts archived complete three-topology
matrices that predate the separate CQ checks. Run it once after
logging into each kernel. Select the report variant explicitly; it is not
inferred from `uname -r`. Use a persistent result root so it remains available
after switching kernels:

```sh
XDS_KERNEL_VARIANT=kasan \
XDS_RESULT_ROOT=$HOME/xds-mem-reg-results \
KSRC=/home/xds/oe_knl \
./test/run_all_tests_in_vm.sh
```

After booting and logging into the non-KASAN VM or kernel, use the same result
root:

```sh
XDS_KERNEL_VARIANT=nokasan \
XDS_RESULT_ROOT=$HOME/xds-mem-reg-results \
KSRC=/home/xds/oe_knl \
./test/run_all_tests_in_vm.sh
```

The runner defaults to `/dev/nvme0n1` and `/dev/nvme0n2`; override them with
`XDS_DEV_1` and `XDS_DEV_2`. Each variant directory must be empty before its
run, which prevents results from different runs being mixed. Raw work
directories and suite logs are kept with the copied report inputs.

When both `kasan/` and `nokasan/` are present, generate the detailed Markdown
report directly in either VM:

```sh
XDS_RESULT_ROOT=$HOME/xds-mem-reg-results \
./test/gen_report_in_vm.sh
```

The default output is `report.md` under `XDS_RESULT_ROOT`. Set
`XDS_REPORT_PATH` to choose another Markdown output path. If the two variants
were run in separate VMs without shared storage, copy the complete `kasan/`
and `nokasan/` directories under one result root before generating the report.

## Remote test runner

The remote VM helper scripts read machine-specific settings from an ignored
local file. Create it from the checked-in example before using either helper:

```sh
cp test/remote_test.env.example test/remote_test.env
chmod 600 test/remote_test.env
${EDITOR:-vi} test/remote_test.env
```

Then start the prepared VM and run the basic suite from the local checkout:

```sh
./test/launch_remote_vm.sh
./test/run_remote_test.sh
```

Set `XDS_REMOTE_TEST_ENV` to use a configuration file at another path. The
configuration is sourced as shell syntax, so it must be trusted. SSH
authentication uses the local SSH agent or SSH configuration for the remote
host and the configured key file for the guest; passwords should not be stored
in the configuration file.

## Dual-kernel matrix and detailed report

`run_dual_kernel_matrix.sh` runs `basic_test.sh` plus RAID0 dynamic-VA stress
and bounded RAID0 CQ checks under exact non-KASAN and KASAN kernel versions. The stress runs
always use 16 iterations. Before building the XDS modules, the runner restores
the running kernel config from `/proc/config.gz`, with
`/boot/config-$(uname -r)` as a fallback, and verifies the resulting module
vermagic. It also reloads NVMe with `use_cmb_sqes=0`.

Build and install both guest kernels when needed:

```sh
./test/build_guest_kernels.sh
```

Then run the complete matrix:

```sh
./test/run_dual_kernel_matrix.sh
```

Set `XDS_MATRIX_BUILD_KERNELS=1` to build the kernels at the beginning of the
matrix command. `XDS_KERNEL_BASE_VERSION` defaults to `6.6.0`. Set
`XDS_RESULT_DIR` to choose the local artifact directory.

Artifacts remain separated under `nokasan/` and `kasan/`. The runner produces
three combined reports:

- `report.json` contains the kernel and suite hierarchy plus every case.
- `report.md` contains a human-readable table for every case.
- `cases.tsv` contains one row per case with kernel, API, memory mode, I/O
  mode, return result, and CRC result.

Report generation rejects a missing kernel, empty or failed suites, kernel-log
issues, mismatched case sets between kernels, incomplete stress iterations,
and stress cases without CRC verification. Its focused local checks are:

```sh
python3 ./test/report_test.py
bash ./test/run_dual_kernel_matrix_test.sh
```
