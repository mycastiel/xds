# Registered-memory UAPI and userspace API

## 1. Goals

The API allows a process to register an HBM range once and reuse its handle and
numeric VA for later `read_file()` calls through any p2p fd in that process.

The chosen interface has these properties:

- registration has an explicit owner p2p fd;
- the returned memory handle is globally indexed but process-scoped for reads;
- explicit unregister is scoped to the owner process and p2p open file
  description;
- `p2p_iov.addr` remains the registering process's numeric VA;
- one memory handle applies to every IOV in one read request;
- bit 0 of an `unsigned long` read-flags field selects registered memory; and
- unregister is synchronous.

## 2. Kernel UAPI

### 2.1 Registration

Add these structures to `p2p_dev_uapi.h`:

```c
struct p2p_mem_register_param {
	/* Exact VA range cached for later reads. */
	__u64 addr;
	__u64 size;
	/* Must be zero. */
	__u64 reserved;
	/* Output on success. */
	__u64 mem_handle;
};

struct p2p_mem_unregister_param {
	/* Must be zero. */
	__u64 reserved;
	__u64 mem_handle;
};

#define IOCTL_REGISTER_MEM \
	_IOWR('k', 4, struct p2p_mem_register_param)
#define IOCTL_UNREGISTER_MEM \
	_IOW('k', 5, struct p2p_mem_unregister_param)
```

Command numbers 1 through 3 remain `ADD_TOPO`, `READ_FILE`, and `DRAIN_IO`.

The registration ioctl obtains the owner identity from `current`; it does not
accept a PID.  This prevents a caller from registering another process's VA
range.  XDS captures the complete `devmm_svm_process_id` needed by the matching
put operation and retains `struct pid *owner_tgid` to authorize registered I/O
and explicit unregister.

The caller supplies the exact range it intends to export.  XDS may align the
range internally to the device page size for devmm, but range validation during
I/O uses the exact unaligned interval supplied here.

The 8-byte `reserved` field in both structures must be zero.  A nonzero value
returns `-EINVAL`.  Registration has no flags in this version.

### 2.2 Read request

Extend the current request with read flags and one handle:

```c
#define P2P_IO_F_REGISTERED_MEM (1UL << 0)
#define P2P_IO_F_MASK           P2P_IO_F_REGISTERED_MEM

struct p2p_read_param {
	int file_fd;
	int host_pid;
	unsigned long flags;
	__u64 mem_handle;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	unsigned int ext_nr;
	struct fiemap_extent extents[];
};
```

The mode is selected as follows:

| Registered-memory flag | Required handle | Address interpretation | PA acquisition |
| --- | --- | --- | --- |
| clear | ignored | VA in the process selected by `host_pid` | Per-read devmm get/put path |
| set | `mem_handle != 0` | Numeric VA in the registering process | Cached registration PA table |

The kernel rejects `flags & ~P2P_IO_F_MASK` with `-EOPNOTSUPP`.  Normal mode
does not inspect `mem_handle`.  Registered mode requires a nonzero handle.  The
handle value itself never selects the mode.

For a registered read, `host_pid` is ignored and userspace must set it to zero.
The kernel compares `task_tgid(current)` with the registration's saved
`owner_tgid`; a task in another process receives `-EPERM`.  In normal mode,
`host_pid` retains its current validation and meaning.

All IOVs in a registered request must be contained in the same registered
range.  The request cannot mix registered and unregistered IOVs or refer to
several handles.  Callers use several asynchronous `read_file()` submissions
when a transfer spans registrations.

### 2.3 Address semantics

For a registered request:

```text
registered start <= iov.addr
iov.size > 0
iov.addr + iov.size does not overflow
iov.addr + iov.size <= registered start + registered size
```

`iov.addr` is not an offset.  It is the original registered VA, including any
displacement within the range.  The registered path treats it only as a
coordinate into the cached PA table; it does not dereference the address while
issuing I/O.

The existing sector-alignment requirements on IOV address and size remain in
force.  Page alignment is not required at read time because the cached PA
translator handles the first and last page offsets.

## 3. Userspace C API

Extend `file_p2p/file_p2p_api.h` with:

```c
#include <stdint.h>

struct read_parameter {
	const char *file_name;
	unsigned long file_offset;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	/* Bit 0 selects registered memory; all other bits are reserved. */
	unsigned long flags;
	/* Meaningful only when P2P_IO_F_REGISTERED_MEM is set. */
	uint64_t mem_handle;
};

int register_mem(int dev_fd, struct p2p_mem_register_param *param);
int unregister_mem(int dev_fd,
		   const struct p2p_mem_unregister_param *param);
```

`register_mem()` passes `p2p_mem_register_param` directly to
`IOCTL_REGISTER_MEM` and leaves the returned nonzero handle in
`param->mem_handle` on success.

The caller initializes `addr`, `size`, and `reserved`, with `reserved == 0`.
The kernel validates nonzero size and checked range arithmetic.

`unregister_mem()` passes `p2p_mem_unregister_param` directly to the synchronous
unregister ioctl.  Returning success means no accepted hardware I/O still uses
the registration and the matching devmm put has completed.

`read_file()` remains the public read function.  It copies
`read_parameter.flags` into the kernel request.  When
`P2P_IO_F_REGISTERED_MEM` is clear, it sets `host_pid = getpid()` and sets the
kernel request's unused handle field to zero.  When the bit is set, it requires
a nonzero `read_parameter.mem_handle`, copies it, and sets `host_pid = 0`.

No special `read_registered_file()` API is added.  The flags and handle are
destination memory metadata, while filename, file offset, FIEMAP handling,
topology lookup, asynchronous submission, and drain semantics are common to
both modes.

## 4. Python API

The Python extension exposes the same model:

```python
handle = file_p2p.register_mem(dev_fd, addr, size)
ret = file_p2p.read_file(
    dev_fd, path, file_offset, iovs,
    handle, file_p2p.P2P_IO_F_REGISTERED_MEM,
)
ret = file_p2p.unregister_mem(dev_fd, handle)
```

`register_mem()` returns the handle on success or `None` on failure.  Python
represents the opaque 64-bit handle as a signed integer, so a valid handle may
be negative when bit 63 is set.  `read_file()` and `unregister_mem()` convert it
back to the same unsigned 64-bit pattern and return zero on success or a
negative errno.  The `mem_handle` and `flags` arguments are required in that
order; normal-memory callers pass zero for both.

## 5. Process and fd scope

The handle is process-scoped even though lookup uses the module-wide registry.
The registering process may issue registered reads through an independently
opened p2p fd.  A different process cannot use the handle for registered I/O.

Explicit unregister is different: the ioctl fd must refer to the owner open
file description and the caller must belong to the registering thread group.
A different p2p fd or process returns `-EPERM`.  A duplicate fd remains valid
in the registering process.  An inherited or transferred owner-fd reference
can keep the registration alive but does not authorize the receiving process
to read or explicitly unregister it.

## 6. Handle validation and errors

The 64-bit handle layout is:

```c
#define P2P_MEM_COOKIE_SHIFT 48
#define P2P_MEM_ID_MASK       GENMASK_ULL(47, 0)
```

Generation takes a 16-bit value from `get_random_u32()` and the low 48 bits of
one global `atomic64_t` sequence.  IDs are not recycled, equal random prefixes
are allowed, and the complete handle is used directly as the XArray index.

Handle zero is invalid when `P2P_IO_F_REGISTERED_MEM` is set.  It does not
select normal memory.  The global sequence starts at one.

Recommended error behavior is:

| Condition | Error |
| --- | --- |
| Any unknown read-flag bit is set | `-EOPNOTSUPP` |
| Registered-memory flag set with zero handle | `-EINVAL` |
| Zero size or arithmetic overflow | `-EINVAL` or `-EOVERFLOW` |
| Unsupported page type or devmm page query | Underlying devmm errno |
| PA-list allocation too large | `-E2BIG` or `-ENOMEM` |
| Unknown or previously removed handle | `-ENOENT` |
| Registration is being unregistered | `-ESTALE` |
| IOV outside the exact registered range | `-ERANGE` |
| Registered read through another p2p fd in the same process | Success if otherwise valid |
| Registered read from another process | `-EPERM` |
| Unregister through a non-owner p2p fd | `-EPERM` |
| Unregister from another process through the owner fd | `-EPERM` |
| Unregister of an already removed handle | `-ENOENT` |

No error path may submit a partial read after registered-memory validation
fails.
