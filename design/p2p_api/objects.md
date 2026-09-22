# NDS I/O objects and topologies

> **Status:** Historical proposal. The authoritative implemented v1 contract
> is [fix3](fix3.md); conflicting PATH-object and topology descriptions below
> are superseded.

This document is part of the [p2p_api](../p2p_api.md) design.  It must not be
implemented in isolation from the [UAPI](uapi.md), [backend](backend.md),
[completion](completion.md), or [testing](testing.md) contracts.

## 1. Goals

Describe how `nds_io_obj` names the storage target for each I/O, how path vs
fd resolution works, which file/bdev operations v1 supports, and how XDS
topology registration is performed automatically without a GDS counterpart.

## 2. Object types

| `type` | Payload | v1 support |
| --- | --- | --- |
| `NDS_IO_OBJ_FD` | `.fd` open descriptor | yes |
| `NDS_IO_OBJ_PATH` | `.str` NUL-terminated path | yes |
| `NDS_IO_OBJ_ID` | `.id` opaque key | no → `-EOPNOTSUPP` |

`pad2` must be zero.  Nonzero `pad2` → `-EINVAL`.
`host_pid` identifies the process owning one-shot HBM VAs (`0` = current);
it must be zero with `NDS_IO_F_REGISTERED_MEM`.

Unknown `type` values → `-EOPNOTSUPP`.

## 3. Operation matrix

| Opcode | Regular file | Block device |
| --- | --- | --- |
| `NDS_IO_OP_PREAD` / `PREADV` | supported | supported |
| `NDS_IO_OP_PWRITE` / `PWRITEV` | supported (raw semantics) | supported |

As in `rw_file`, the library preallocates regular-file writes before
FIEMAP. The kernel accesses raw extents without page-cache coherency or
unwritten-extent conversion.

`offset` is a byte offset: logical file offset for regular-file I/O, raw
byte offset for block devices.

## 4. PATH vs FD resolution

### 4.1 `NDS_IO_OBJ_FD`

- The descriptor must refer to a regular file or a block device.
- Read opens are expected readable; write requires a writable regular-file
  or block-device fd.
- The library does not take ownership of the caller's fd; it may `dup` or
  pass the fd number into the existing ioctl packing path as today's
  `file_fd` field requires.  Prefer the same lifetime model as `rw_file`:
  the fd must remain valid through accept; kernel `fget` holds it for the
  I/O context until drain/retirement.

### 4.2 `NDS_IO_OBJ_PATH`

Chosen v1 behavior (locked):

1. Library opens the path with `O_RDONLY` (read) or `O_WRONLY` (write) and
   `O_CLOEXEC`.
2. On open failure, submit returns `-errno` for that request.
3. The open fd is used for extent prep / ioctl `file_fd` exactly as
   `rw_file(file_name=...)` does today.
4. The library closes its open fd after the ioctl accept path releases it (same
   as current `rw_file` closing after ioctl), without exposing the temporary
   fd to the caller.

Do not invent a long-lived path cache that outlives the request unless a later
design adds handle register (explicitly NA for now).

## 5. Automatic topology registration

GDS has no topology API.  XDS requires `IOCTL_ADD_TOPO` before I/O on a top
device.  NDS hides this:

```text
submit resolves target bdev (from file's sb_bdev or the block fd)
  → if not cached for this ctx (or process):
        add_topo(p2p_fd, bdev_path_or_node)
  → on success, cache identity (dev_t)
  → on failure, fail the iocb with that errno
```

### 5.1 Cache scope

v1 caches successful topo registrations **per NDS ctx** (tied to that ctx's
p2p fd).  A new ctx may re-add the same top device; the kernel first-wins pin
behavior remains acceptable and matches current XDS usage.

### 5.2 Supported layouts

Only what `add_topo` / `topo.c` already support:

- direct NVMe namespace or partition (`nvme`);
- dm-linear (`linear`);
- MD RAID0 (`raid0`).

Multipath NVMe and other MD levels remain `-EOPNOTSUPP`.

### 5.3 Optional explicit API

An XDS-only `nds_add_topo(ctx, path)` may be added later as a usability helper
for preflight.  It is not part of the GDS comparison table and is not required
for v1 if automatic registration on submit is implemented.

## 6. Interaction with memory modes

Object resolution is independent of registered vs one-shot HBM mode.  After
the target is known and topo is ensured, the backend applies the memory-mode
translation from [uapi.md](uapi.md) / [backend.md](backend.md).

## 7. Multi-semantic evolution

`NDS_IO_OBJ_ID` reserves space for KV/object stores.  v1 must reject it with
`-EOPNOTSUPP` before open or topo work.  Future designs may define ID namespaces
without changing `nds_io_cb` layout beyond documenting `type` values.

## 8. Reliability notes

- Never skip `add_topo` because a previous process succeeded; the ctx fd must
  pin topo for its own I/O.
- Surface topo/discovery errors at submit; do not convert them into empty
  completions.
- PATH opens must not leave fds leaked on validation failure after open.
- Write path must re-check block-device type after open (TOCTOU-safe as far as
  `rw_file` already is).

## 9. Non-goals

- cuFile-style persistent file-handle register/deregister.
- Filesystem write support.
- Monitoring dm/md reshape; callers must not change topology under inflight
  I/O (same XDS contract).
