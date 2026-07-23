# XDS prepared-VM tests

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

The sequence is:

1. Direct block-device reads and writes on both namespaces, followed by ext4
   read coverage with 1K, 2K, and 4K blocks on the first namespace.
2. A partition on the first NVMe namespace, tested for raw reads and writes
   and with all three ext4 block sizes for reads, then removed before
   stacked-device testing.
3. A 126 MiB dm-linear device made from the 63 MiB usable region of each
   namespace. Each segment starts at a 1 MiB underlying-device offset to match
   the real LVM data layout. It is tested for raw reads and writes and with all
   three ext4 block sizes for reads, and is created and removed directly with
   `dmsetup`.
4. The same dm-linear coverage with equal-sized partitions starting at
   different namespace sectors as its components.
5. A two-member, 64 KiB-chunk MD RAID0, tested for raw reads and writes and
   with all three ext4 block sizes for reads.
6. A two-member RAID0 created with the namespace arguments reversed, verifying
   topology discovery orders components by MD slot rather than sysfs directory
   enumeration order.
7. The same RAID0 coverage with equal-sized partitions starting at different
   namespace sectors as its members. The MD member offsets and component sizes
   remain equal.

Every filesystem phase runs individual boundary cases, four queued reads of
different files, and four simultaneous same-fd submissions. The C and Python
APIs use separate CMB ranges.

For each raw topology, both APIs run a write pipeline that first places a
deterministic pattern into a seed range with `O_DIRECT`, reads it into three
HBM IOVs through XDS, drains and validates the read-side HBM CRC, then writes
the unchanged IOVs to a distinct raw range. An `O_DIRECT` host read verifies
the entire payload plus known guards immediately before and after it. The
transfer crosses CMB-page, queue-limit, and topology boundaries. Write-only
kernel logs are checked to ensure that read-only CRC and PA-content diagnostics
were not emitted.

The first namespace also runs the write pipeline with registered memory for
both APIs. Separate rejection tests cover invalid operations, registration
reserved fields, unknown flags, regular-file writes, read-only block-device
descriptors, insufficient extent coverage, and ranges beyond device capacity.

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

Both the C and Python APIs run normal-memory and registered-memory variants
against the same generated workload. A registered run pins the whole CMB once
through a dedicated registration fd before starting the worker threads. Each
worker issues I/O through its own distinct fd, and the run verifies one PA-list
get and put. Each variant checks its runtime allocation manifest,
kernel/userspace CRC32 results, and debugfs failure/inflight counters. Set
`XDS_STRESS_ITERATIONS` to change the default 16 iterations and
`XDS_STRESS_SEED` to replay another workload; the default seed is `0x584453`.
The common `XDS_TEST_WORKDIR`,
`XDS_TEST_KEEP_WORKDIR`, `XDS_TEST_BUILD_JOBS`, and `KSRC` controls apply to
both entry points. Shared prepared-VM and storage helpers live in `common.sh`.

## Running the complete matrix inside the VM

`run_all_tests_in_vm.sh` runs the basic suite and the RAID0, dm-linear, and
direct-NVMe stress suites without SSH or rebooting the VM. Run it once after
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

`run_dual_kernel_matrix.sh` runs `basic_test.sh` plus all three stress
topologies under exact non-KASAN and KASAN kernel versions. The stress runs
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
