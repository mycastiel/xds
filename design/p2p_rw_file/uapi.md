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
`file_offset` is a logical file offset for regular-file I/O and a raw
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
	__u64 user_data;
	__u64 iov;       /* userspace pointer to p2p_iov[] */
	unsigned int iov_nr;
	unsigned int ext_nr;
	__u64 extents;   /* userspace pointer to fiemap_extent[] */
	__u64 reserved[3];
};
```

`IOCTL_SUBMIT_IO` accepts a batch of these fixed-size records. Each
accepted record produces one completion event. The kernel copies the
pointed-to IOVs and extents before returning from submission. File-write
support does not change the request layout or the batch acceptance
contract.

## 3. Target and extent contract

The operation selects the permitted target and extent source:

| Operation | Regular-file preparation | Block-device preparation |
| --- | --- | --- |
| Read | FIEMAP | One synthetic extent |
| Write | fallocate, then FIEMAP | One synthetic extent |

A synthetic block-device extent has equal logical and physical offsets and a
length equal to the total IOV size:

```text
fe_logical  = file_offset
fe_physical = file_offset
fe_length   = sum(iov[].size)
```

The library emits one synthetic extent for its contiguous block-device
requests. Direct ioctl callers may supply multiple extents for either
direction. IOV bytes are mapped to physical extents in array order;
`fe_logical` is unused for block devices. Completion order is unspecified,
so callers must avoid overlapping write destinations within a request.

The same pointed-to extent array is used for both operations. Regular-file
writes use library preallocation followed by FIEMAP; direct ioctl callers
are responsible for preparing allocated, stable mappings. Both directions
accept unwritten extents under the raw contract in
[io_path.md](io_path.md).

Userspace opens the target according to the operation.  The kernel
independently validates the operation, inode type, file access mode, IOVs,
extent coverage, alignment, overflow, device capacity, and topology before
issuing I/O.  A write requires a write-capable regular-file or block-device
reference.

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
