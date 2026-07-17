# XDS prepared-VM tests

`basic_test.sh` is a destructive end-to-end test for the XDS direct-read path.
It builds the kernel modules and both userspace APIs, uses a QEMU NVMe
Controller Memory Buffer as the destination, and verifies every successful
read by comparing kernel and userspace CRC32 values.

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

1. Direct block-device reads on both namespaces, followed by ext4 with 1K,
   2K, and 4K blocks on the first namespace.
2. A partition on the first NVMe namespace, tested as a block device and with
   all three ext4 block sizes, then removed before stacked-device testing.
3. A 126 MiB dm-linear device made from the 63 MiB usable region of each
   namespace. Each segment starts at a 1 MiB underlying-device offset to match
   the real LVM data layout. It is tested as a block device and with all three
   ext4 block sizes, and is created and removed directly with `dmsetup`.
4. The same dm-linear coverage with equal-sized partitions starting at
   different namespace sectors as its components.
5. A two-member, 64 KiB-chunk MD RAID0, tested as a block device and with all
   three ext4 block sizes.
6. A two-member RAID0 created with the namespace arguments reversed, verifying
   topology discovery orders components by MD slot rather than sysfs directory
   enumeration order.
7. The same RAID0 coverage with equal-sized partitions starting at different
   namespace sectors as its members. The MD member offsets and component sizes
   remain equal.

Every filesystem phase runs individual boundary cases, four queued reads of
different files, and four simultaneous same-fd submissions. The C and Python
APIs use separate CMB ranges.

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

Both the C and Python APIs run against the same generated workload. Each run
checks its runtime allocation manifest, kernel/userspace CRC32 results, and
debugfs failure/inflight counters. Set `XDS_STRESS_ITERATIONS` to change the
default 16 iterations and `XDS_STRESS_SEED` to replay another workload; the
default seed is `0x584453`. The common `XDS_TEST_WORKDIR`,
`XDS_TEST_KEEP_WORKDIR`, `XDS_TEST_BUILD_JOBS`, and `KSRC` controls apply to
both entry points. Shared prepared-VM and storage helpers live in `common.sh`.
