# p2p_dev: Derive Block Device From file_fd

## Summary

Resolve the target block device from the file fd inside `p2p_dev.c` instead of
requiring a separate block-device fd as the primary source of truth.

## Current Gap

The current read ioctls accept both `file_fd` and `bdev_fd`, but `p2p_dev.c`
uses `bdev_fd` to select the disk. This lets userspace pass a file from one
filesystem and an unrelated block device, which can make FIEMAP offsets and the
request queue disagree.

## Proposal

- Use `file_fd` as the primary source for the target block device.
- If `file_fd` refers to a block device, resolve the bdev from the file inode.
- If `file_fd` refers to a regular file, resolve the bdev from
  `file_inode(file)->i_sb->s_bdev`.
- Reject files whose superblock has no single block device with `-EOPNOTSUPP`.
- Remove `bdev_fd` from the read UAPI structs and from the userspace setup path.
  Userspace should no longer open or pass a separate block-device fd for file
  reads.
- Store the resolved bdev in the IO context only after validation.
- Pin the source file instead of taking a separate block-device reference.
  Keep the `struct file *` returned by `fget(desc.file_fd)` in the IO context
  until async IO is complete, then release it from `free_io_ctx()` with
  `fput(io_ctx->file)`.
- Do not use `bdgrab()` or `bdput()`. The `oe_knl` block layer uses
  `bdev_handle`, and this plan avoids depending on either block-device lifetime
  API by pinning the file that owns the derived bdev.

## Rationale

FIEMAP extents are meaningful only for the file's own backing block device.
Deriving the bdev from `file_fd` prevents userspace from accidentally pairing a
file with the wrong disk.

## Acceptance Criteria

- Regular-file reads no longer require userspace to provide a separate bdev.
- Block-device reads still work when `file_fd` itself is a block device.
- The read UAPI no longer contains `bdev_fd`.
- The async IO path does not use a bdev after dropping the source-file
  reference that pins it.
