# file_p2p_api: Filesystem Block Sizes Below 4 KiB

## Summary

Update `file_p2p_api.c` so FIEMAP preparation works on filesystems whose
logical block size is smaller than 4 KiB, such as 512 B, 1 KiB, and 2 KiB.

## Current Gap

`file_p2p_api.c` assumes 4 KiB in several places:

- `max_num = max_size >> 12`
- offset trimming uses `% 4096`
- FIEMAP allocation can undercount extents when the filesystem block size is
  smaller than 4 KiB

This can produce too few FIEMAP slots or incorrect trimming on sub-4K
filesystems.

## Proposal

- Determine the filesystem block size for regular files before FIEMAP.
- Prefer `ioctl(file_fd, FIGETBSZ, &block_size)`.
- Validate that the returned block size is a power of two and at least 512.
- Fall back to 4096 only if the filesystem does not report a usable value.
- Replace all hard-coded 4096 arithmetic with the detected block size.
- Compute FIEMAP extent capacity with a round-up formula:
  - include the starting offset inside the filesystem block,
  - divide by the detected block size,
  - always allocate at least one extent slot.
- Align the FIEMAP query start down to the detected block size.
- Expand the FIEMAP query length enough to cover the requested byte range.
- Trim returned extents back to the requested byte range before issuing the
  ioctl to `p2p_dev.ko`.

## Extent Field Contract

The implementation must keep the userspace and kernel interpretation of
`struct fiemap_extent` consistent. The disk byte offset passed to `p2p_dev.c`
must be the adjusted physical offset from FIEMAP, not the file logical offset.
If `p2p_dev.c` continues to consume `fe_logical`, `file_p2p_api.c` must
normalize that field before the ioctl. If `p2p_dev.c` is fixed to consume
`fe_physical`, no userspace field overloading is needed.

## Acceptance Criteria

- Reads work on filesystems with 512 B, 1 KiB, 2 KiB, and 4 KiB block sizes.
- FIEMAP extent allocation does not truncate the mapping list for sub-4K
  filesystems.
- Offset trimming uses the detected filesystem block size, not 4096.
- Block-device input still uses the requested block-device offset directly and
  does not require FIEMAP.
