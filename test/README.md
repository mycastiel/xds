# XDS basic KVM test

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

The script rejects mounted devices, active holders, partitions, incorrect
sizes, different controllers, equal NSIDs, an unexpected BAR2 size, or active
CMB SQ usage. It then destroys all existing data on both namespaces.

Set `XDS_TEST_WORKDIR` to choose the parent for a unique runtime results
directory. Set
`XDS_TEST_KEEP_WORKDIR=1` to retain logs and generated manifests after a
successful run. Failure artifacts are always retained.

The sequence is:

1. Direct block-device reads on both namespaces, followed by ext4 with 1K,
   2K, and 4K blocks on the first namespace.
2. A 126 MiB dm-linear device made from the 63 MiB usable region of each
   namespace. Each segment starts at a 1 MiB underlying-device offset to match
   the real LVM data layout. It is tested as a block device and with all three
   ext4 block sizes, and is created and removed directly with `dmsetup`.
3. A two-member, 64 KiB-chunk MD RAID0, tested as a block device and with all
   three ext4 block sizes.

Every filesystem phase runs individual boundary cases, four queued reads of
different files, and four simultaneous same-fd submissions. The C and Python
APIs use separate CMB ranges.
