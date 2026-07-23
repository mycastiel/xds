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
    regular file -> -EOPNOTSUPP
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
| Target type | Regular file or block device | Block device only |
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

The caller is responsible for exclusive logical ownership of the destination
range.  Rejecting regular-file writes is the boundary that prevents the first
write implementation from implying filesystem semantics it cannot provide.
