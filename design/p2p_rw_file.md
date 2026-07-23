# XDS unified read/write I/O

## 1. Scope

This document is the entry point for adding write support to the existing XDS
read path.  The design deliberately keeps one userspace operation, one kernel
ioctl, one kernel I/O engine, and one drain operation for both directions.

The first write implementation supports block devices only.  A write directed
at a regular file is rejected with `-EOPNOTSUPP`.  XDS does not allocate file
blocks, convert unwritten extents, or maintain filesystem and page-cache
coherency for writes.

Read behavior remains unchanged: reads may use regular files or block devices.
Both directions retain the existing topology mapping, physical-memory IOV,
registered-memory, asynchronous completion, and per-p2p-fd batching models.

These documents define only the core design principles.  They do not prescribe
an implementation sequence or internal refactoring steps.  The testing
sub-design defines the required validation model without prescribing a test
implementation.

## 2. Sub-design documents

- [Unified UAPI and userspace API](p2p_rw_file/uapi.md) defines `rw_file()`,
  `IOCTL_RW_FILE`, operation selection, and the shared parameter structures.
- [Shared kernel I/O path](p2p_rw_file/io_path.md) defines the common
  extent-based engine and the narrow set of direction-specific behavior.
- [Drain, ordering, and buffer lifetime](p2p_rw_file/drain.md) defines mixed
  read/write batching, completion semantics, ordering, and diagnostics.
- [Write testing](p2p_rw_file/testing.md) defines how known data is loaded into
  HBM, how the block-device result is verified independently, and the required
  topology, drain, and error coverage.

The documents form one design.  The API, I/O path, drain, and testing contracts
must remain consistent with each other.

## 3. Core principles

1. `rw_file()` and `IOCTL_RW_FILE` are the only file-I/O entry points.  An
   explicit operation field selects read or write and is the first field in
   both shared parameter structures.
2. Reads support regular files and block devices.  Writes support block devices
   only, and the kernel is the authoritative enforcement point.
3. `struct fiemap_extent` remains the common disk-range representation.  The
   design does not introduce a filesystem-independent internal range type.
4. Read and write share memory translation, extent traversal, topology mapping,
   request splitting, asynchronous context ownership, completion accounting,
   and drain handling.
5. Direction-specific behavior is limited to access validation, NVMe and block
   request direction, data-buffer semantics, direction-aware logging, and
   read-only CRC and content diagnostics.
6. `drain_io()` snapshots and waits for both read and write contexts published
   on the same p2p fd.  It remains the boundary after which buffers may be
   consumed, modified, released, or reused.
7. Raw block-device writes provide no filesystem-coherency guarantee.  The
   caller must own the target range and must not write beneath a mounted or
   otherwise active filesystem.
8. Testing first populates HBM from known raw block-device data through the XDS
   read path, then verifies the XDS write through an independent host
   block-device read-back.  Kernel CRC and PA-content dumping are not performed
   for writes and are not a write-verification mechanism.
