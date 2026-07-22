# p2p_dev: NVMe Disk Identification

## Summary

Validate the block device in `p2p_dev.c` before issuing P2P NVMe reads. Use
sysfs as the only identification mechanism, and reject NVMe multipath heads
because `p2p_dev.ko` issues direct NVMe requests while the
multipath head is bio-based. Partition bdevs are also rejected; `p2p_dev.ko`
only supports a direct whole-namespace/path bdev.

## Current Gap

`p2p_dev.c` currently obtains a block device from `bdev_fd` and then submits
NVMe requests to `bdev->bd_disk->queue`. It does not confirm that the disk is a
supported direct NVMe namespace/path. Passing a SCSI disk or an NVMe multipath
head can reach the NVMe request path incorrectly.

Partition-backed bdevs are not supported. The module issues direct NVMe
commands to the namespace queue and does not rely on block-layer partition
remapping, so accepting a partition would require a separate LBA translation
policy. The intended behavior is to reject the partition bdev instead.

## Proposal

- Add a `p2p_validate_nvme_bdev()` helper in `p2p_dev.c`.
- Call the helper after resolving `struct block_device *bdev` in both single
  and batch read paths.
- Reject partition bdevs before sysfs classification.
- Resolve sysfs identity for the passed block device using
  `/sys/dev/block/<major>:<minor>`, where the major/minor come from
  `bdev->bd_dev`.
- First check `/sys/dev/block/<major>:<minor>/device/subsysnqn`.
  - If missing, sysfs did not identify this bdev as a supported direct NVMe
    namespace/path. Reject it.
  - If present, treat the bdev as an NVMe candidate.
- Then check `/sys/dev/block/<major>:<minor>/device/iopolicy`.
  - If present, reject with `-EOPNOTSUPP` because this is an NVMe multipath
    head.
  - If absent, accept the bdev as a supported direct NVMe namespace/path.
- Do not check `device/transport`.
- Do not use procfs.
- Do not use module-owner or disk-name fallback checks.
- Do not modify `nvme-core`.
- Do not cast `gendisk->private_data` to NVMe private structs.

## No Fallback

Fail closed when sysfs classification is unavailable. The validator must not
accept a disk because of its module owner, disk name, or major number.

## Behavior

- Direct whole NVMe namespace/path bdev: `subsysnqn` present, `iopolicy` absent,
  accepted.
- NVMe namespace/path partition bdev: rejected because it does not expose the
  direct namespace sysfs identity expected by `p2p_dev.ko`; no partition-start
  LBA remap is performed.
- NVMe multipath head bdev: `subsysnqn` present, `iopolicy` present, rejected
  with `-EOPNOTSUPP`.
- SCSI or non-NVMe bdev: `subsysnqn` absent, rejected before
  `nvme_init_request()`.

## Acceptance Criteria

- Unsupported disks are rejected before any NVMe request allocation.
- Partition bdevs are rejected before any NVMe request allocation.
- NVMe multipath heads are rejected with a clear log message.
- The logic is fully contained in `p2p_dev.c`.
- No `nvme-core` patch or private NVMe struct cast is required.
