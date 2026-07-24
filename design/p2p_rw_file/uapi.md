# Unified read/write UAPI and userspace API

## 1. One userspace operation

The C API exposes one asynchronous I/O operation:

```c
#define P2P_IO_READ  0U
#define P2P_IO_WRITE 1U

struct io_parameter {
	unsigned int op;
	const char *file_name;
	unsigned long file_offset;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	unsigned int flags;
	int host_pid;
	uint64_t mem_handle;
};

int rw_file(int dev_fd, const struct io_parameter *param);
```

`op` is the first field and must be exactly `P2P_IO_READ` or
`P2P_IO_WRITE`.  Direction is a fundamental property of the operation and is
not encoded in `flags`.  The flags remain independent modifiers such as
registered-memory selection.

`host_pid` follows `flags`.  For unregistered memory, it identifies the process
whose virtual addresses appear in `iov`.  Zero selects the calling process; a
positive value selects that PID.  Negative values are invalid.
Registered-memory requests must set `host_pid` to zero because the registration
already identifies the address-space owner.

For a read, the IOVs are destinations.  For a write, the IOVs are sources.
`file_offset` is a logical file offset for a regular-file read and a raw
block-device byte offset for block-device I/O.

## 2. One kernel ioctl

The kernel UAPI uses the same operation values and one request structure:

```c
struct p2p_io_param {
	unsigned int op;
	unsigned int flags;
	int file_fd;
	int host_pid;
	__u64 mem_handle;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	unsigned int ext_nr;
	struct fiemap_extent extents[];
};

#define IOCTL_RW_FILE _IOWR('k', 2, struct p2p_io_param)
```

`op` is the first field.  Unknown operation values and unknown flag bits are
rejected before any hardware request is submitted.
`IOCTL_RW_FILE` replaces the direction-specific read or write ioctls; there is
no separate `IOCTL_READ_FILE` or `IOCTL_WRITE_FILE` contract.

## 3. Target and extent contract

The operation selects the permitted target and extent source:

| Operation | Regular file | Block device | Extent preparation |
| --- | --- | --- | --- |
| `P2P_IO_READ` | Supported | Supported | FIEMAP for a regular file; one synthetic extent for a block device |
| `P2P_IO_WRITE` | `-EOPNOTSUPP` | Supported | One synthetic extent for the requested block-device range |

A synthetic block-device extent has equal logical and physical offsets and a
length equal to the total IOV size:

```text
fe_logical  = file_offset
fe_physical = file_offset
fe_length   = sum(iov[].size)
```

The same flexible extent array is therefore valid for both operations.  Write
support does not add FIEMAP or filesystem-allocation behavior; it uses the
existing extent representation only to share the kernel traversal path.

Userspace opens the target according to the operation.  The kernel
independently validates the operation, inode type, file access mode, IOVs,
extent coverage, alignment, overflow, device capacity, and topology before
issuing I/O.  A write requires a write-capable block-device file reference.

## 4. Memory-mode contract

The existing one-shot and registered-memory modes apply equally to reads and
writes.  One request uses one memory mode and, in registered mode, one memory
handle for every IOV.

The userspace library resolves a zero `io_parameter.host_pid` to `getpid()` for
one-shot I/O before issuing the ioctl.  A nonzero PID is passed through.
Registered-memory I/O rejects a nonzero `host_pid` and sends zero to the
kernel.

The direction changes ownership semantics but not address translation:

- a read pins or references destination memory until drain;
- a write pins or references source memory until drain; and
- registered and unregistered IOVs use the same validation and PA translation
  rules already established for reads.
