# p2p_dev: NVMe Major Handling

## Summary

Do not depend on a global `nvme_major` symbol. Derive the actual major and
minor numbers from the block device selected for the request.

## Current Gap

The requested feature asks for `nvme_major` in the kernel, but modern NVMe block
namespace devices do not need a stable global major for this path. The block
major can be dynamic, and a major number alone does not prove that a `gendisk`
is an NVMe namespace.

## Proposal

- After resolving the request block device, read the device number from
  `bdev->bd_dev`.
- Use `MAJOR(bdev->bd_dev)` and `MINOR(bdev->bd_dev)` for diagnostics and for
  sysfs identity lookup under `/sys/dev/block/<major>:<minor>`.
- Do not use the major number as the NVMe identity check.
- Keep the actual identity decision in the NVMe disk-identification logic.

## Rationale

The real request target is the userspace-passed or file-derived block device.
Its `dev_t` is available without any NVMe-core private symbol. A major number is
useful for logging and sysfs lookup, but it is too weak to distinguish direct
NVMe namespaces, NVMe multipath heads, SCSI disks, and other block devices.

## Acceptance Criteria

- No dependency on a `nvme_major` kernel symbol.
- Logs and sysfs checks use the major/minor from the actual `bdev`.
- SCSI or non-NVMe disks are not accepted merely because of a major-number
  match.
