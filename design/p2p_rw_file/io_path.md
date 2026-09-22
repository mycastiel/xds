# Shared kernel read/write I/O path

## 1. One extent-based engine

The kernel has thin operation validation around one common I/O engine.  The
engine continues to consume `struct fiemap_extent`; it does not introduce a
second disk-range abstraction for writes.

```text
rw_file(P2P_IO_READ)
    regular file -> FIEMAP extents
    block device -> one synthetic extent
                                  \
                                   shared extent-based I/O engine
                                  /
rw_file(P2P_IO_WRITE)
    regular file -> fallocate -> FIEMAP extents
    block device -> one synthetic extent
```

The total ordered IOV length is the transfer length.  Extents provide the
ordered disk ranges, and the existing rule that extent coverage must be at
least the IOV total remains common to both directions.

## 2. Shared behavior

Read and write reuse the same principles for:

- request and flag validation;
- one-shot and registered-memory PA translation;
- `p2p_iov_iter` traversal across physically contiguous PA segments;
- topology lookup, pinning, and logical-to-component sector mapping;
- splitting at extent, topology, queue-limit, and PA-segment boundaries;
- `p2p_io_context` ownership and child-request reference accounting;
- asynchronous batch publication;
- completion-status aggregation; and
- cleanup by `drain_io()` or p2p-fd release.

The common path must not maintain parallel read and write implementations of
this machinery.  The operation travels with the logical I/O context and
selects the narrow direction-specific behavior at request construction and
diagnostic points.

## 3. Direction-specific behavior

The two operations differ only where the direction has real meaning:

| Property | Read | Write |
| --- | --- | --- |
| Memory role | NVMe destination | NVMe source |
| NVMe opcode | `nvme_cmd_read` | `nvme_cmd_write` |
| Block request operation | `REQ_OP_DRV_IN` | `REQ_OP_DRV_OUT` |
| Target type | File or block device | File or block device |
| File access | Read-capable | Write-capable |
| Buffer CRC and content dump | Optional after completion | Not performed |

Kernel-version compatibility helpers preserve these direction choices while
retaining the existing request and NVMe-command ownership rules for each
supported kernel.  Adding write support must not require write permission for
a read-only `rw_file()` operation.  The write-capable top-level file descriptor
authorizes a write and remains held through drain; the topology's component
handles remain lifetime pins, while the NVMe opcode and block request operation
carry the component-I/O direction.

## 4. Raw-device boundary

A block-device write operates on the raw byte range represented by its
synthetic extent.  XDS validates and maps that range but does not provide:

- filesystem block allocation;
- unwritten-extent conversion;
- filesystem journaling;
- page-cache invalidation; or
- coordination with mounted filesystem users.

The caller is responsible for exclusive logical ownership of the
destination range. Regular-file support uses the separate raw storage
contract below; it does not imply ordinary filesystem write semantics.

## 5. Raw file-backed storage

The library validates the operation, buffers, aligned logical range, and
write access before `fallocate(fd, 0, offset, length)`. Mode zero grows the
file when needed; KEEP_SIZE and ZERO_RANGE are not used. Allocation
failures propagate without a truncate or host-copy fallback. Reads and
block devices skip this step. The library then obtains FIEMAP extents and
trims them to the exact request. Mappings must cover the range in logical
order without holes or overlaps. Physical ranges retain the existing sector
alignment constraints.

Both directions accept UNWRITTEN extents. They expose raw device contents,
not filesystem-synthesized zeroes. Only ranges successfully written by XDS
are valid for XDS readback. A failed write can leave partial data. Ordinary
file reads can still return zeros even after successful XDS writes because
there is no unwritten-to-written conversion. fsync and FIEMAP_FLAG_SYNC do
not perform that conversion. Normal file copying/backup is not a reliable
way to preserve this raw payload.

Only LAST, MERGED, UNWRITTEN, and SHARED extent flags are understood.
SHARED is accepted for reads but rejected for writes to avoid bypassing
COW. Unknown, delayed, encrypted, compressed, inline, tail-packed, and
unaligned extents are rejected. The kernel repeats flag and file-range
structural checks and checks capacity for every write extent. Raw
block-device writes still use one synthetic extent. The ioctl does not
allocate space or prove ownership of caller-supplied physical addresses; it
remains a trusted raw-I/O interface.

Callers must exclusively manage the file and keep its mapping stable from
FIEMAP through completion. Do not mix buffered/mmap writes, truncate, hole
punching, reflinking, snapshots that introduce sharing, or extent
relocation with XDS access. Opening a file does not pin its extent mapping.
The module provides no filesystem cache invalidation or mapping lock. Fresh
files used only through XDS are the intended initial use case.

Preparation is synchronous and may block on allocation or metadata I/O.
nds_io_submit prepares a batch before kernel acceptance: even an unaccepted
item can leave allocated space or a larger file. No automatic rollback is
performed. Completion retains the existing non-atomic, non-durable
contract. Large reusable files can be preallocated to amortize allocation
work; this does not authorize caching stale mappings.
