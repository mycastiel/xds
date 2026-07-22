# p2p_dev: Merge `read_file` and `read_file_batch`

## Summary

Complete the vector-based read interface prepared by commit `60ea660` and
replace the duplicated single-address and batch-address implementations in
`p2p_dev.c` with one path.

One kernel request describes one contiguous file range and an ordered list of
destination buffers:

- `iov_nr == 1` is the old `read_file` case.
- `iov_nr > 1` is the old `read_file_batch` scatter case.
- File extent bytes are copied sequentially across the IOVs.
- The sum of extent lengths must be at least the sum of IOV lengths. The IOV
  total is the requested transfer size; any unused extent tail is ignored.

Keep both ioctl command numbers, but route both commands to the same handler.
This preserves the two entry points while removing mode-specific behavior from
the kernel implementation.

## Current State

`HEAD` introduces `struct p2p_iov` and `struct p2p_read_param`, and changes both
read ioctls to use that request type. The conversion is intentionally
incomplete:

- `p2p_dev.c` still consumes `struct read_desc` and `struct read_desc_ba`.
- `get_pa_list()` and `get_pa_list_batch()` represent the same translation in
  incompatible forms.
- `new_io_ctx()` and `new_io_ctx_batch()` duplicate setup and calculate command
  capacity in different units.
- `do_read_ios()` and `do_read_ios_batch()` duplicate the source-extent loop.
- The old batch loop advances beyond the final PA entry on an exact-fit final
  chunk and records `-EINVAL` after the data has already been submitted.
- The old size check uses aligned page capacity, so page padding can satisfy the
  check even when the requested IOV bytes are too short.
- The removed `struct va_desc` is still referenced by the obsolete
  `IOCTL_DUMP_PA`, `paddr_desc`, and `dump_pa()` code, so the current tree does
  not build.
- `file_p2p_api.c` and the Python binding still populate the removed request
  fields.

## Request Contract

Finish `p2p_dev_uapi.h` around the `HEAD` layout:

```c
struct p2p_iov {
	unsigned long addr;
	unsigned long size;
};

struct p2p_read_param {
	int file_fd;
	unsigned int nsid;
	int host_pid;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	unsigned int ext_nr;
	struct fiemap_extent extents[];
};
```

- Remove `va_desc`, `va_desc_ba`, `read_desc`, `read_desc_ba`, `paddr_desc`,
  `IOCTL_DUMP_PA`, and `dump_pa()` after the read users are converted.
- Keep `IOCTL_READ_FILE` encoded with `struct p2p_read_param`.  Remove the
  redundant `IOCTL_READ_FILE_BATCH`; the userspace batch helper submits every
  element through `IOCTL_READ_FILE`.
- Treat this as the native-word-size ABI already established by `unsigned long`
  and the userspace pointer. Compat-ioctl support is outside this change.

Before allocation or address translation, the kernel handler must validate:

- `iov_nr` and `ext_nr` are nonzero and within explicit driver limits.
- Array byte counts do not overflow `size_t`.
- Every IOV has a nonzero length, a sector-aligned address and length, and
  `addr + size` does not overflow. Address zero remains valid for the stub
  mapping used by the current test path.
- Every extent has a nonzero, sector-aligned physical address and length.
- Summing all IOV lengths and all extent lengths does not overflow `u64`.
- The extent byte total is at least the IOV byte total. Reject a sparse or
  truncated mapping that cannot cover every destination byte; do not issue
  data from an extent tail beyond the IOV total.

## Kernel Implementation

### 1. Copy and validate one request

Implement one `p2p_read_file()` handler that:

1. Copies the fixed `p2p_read_param` header from userspace.
2. Copies the IOV array with overflow-checked sizing.
3. Copies the flexible extent array with overflow-checked sizing.
4. Validates the request and uses the IOV total as the exact transfer byte
   count.
5. Acquires `file_fd`, derives and validates the NVMe block device, translates
   the destination IOVs, creates the I/O context, and submits the reads.

Return the real translation or validation errno. Do not convert every
`get_pa_list` failure to `-EFAULT` as the old batch handler does.

### 2. Flatten IOVs into exact PA IOVs

Replace `get_pa_list()` and `get_pa_list_batch()` with one helper that accepts
`host_pid` plus the copied `p2p_iov[]` and returns kernel-only physical IOVs:

```c
struct p2p_pa_iov {
	u64 addr;
	u64 len;
};
```

For each IOV:

1. Query its device-memory page size.
2. Align the virtual range only for the `devmm_get_mem_pa_list()` call.
3. Convert every returned page base into an exact segment by adding the first
   page offset and clipping the first and last lengths to the IOV.
4. Call `devmm_put_mem_pa_list()` on every successfully acquired range.

The output contains no alignment padding. It therefore replaces `pa_list`,
`pa_size`, `addr_off`, and `align_size`, supports the single-IOV case without a
special branch, and does not require every IOV to use the same page size.
Use overflow-checked page and IOV counts, and make the I/O context own the
completed PA IOV array after successful construction.

### 3. Use one I/O context constructor

Replace `new_io_ctx()` and `new_io_ctx_batch()` with one constructor taking the
common request fields, exact data size, source extent count, and destination
segments.

- Initialize timestamps for every request.
- Calculate `max_bytes` from `p2p_queue_max_sectors()` once.
- Allocate `cmd_list` using an overflow-checked upper bound that accounts for
  source-extent boundaries, destination-segment boundaries, and
  `DIV_ROUND_UP(data_size, max_bytes)` splits.
- Store and free the destination PA IOV array in `p2p_io_context`. Traversal
  state remains local to `do_read_ios()` and is not retained for completion or
  drain handling.
- Check `cmd_id` against the allocated count before taking a pointer to
  `cmd_list[cmd_id]`.

This also removes the current byte-versus-sector mismatch between the two
constructor calculations.

### 4. Use one read-issuing state machine

Replace `do_read_ios()` and `do_read_ios_batch()` with one extent loop and a
local `p2p_iov_iter` over the destination PA IOVs. Model its API after the
kernel iterator operations:

- `p2p_iov_iter_init()` initializes the current IOV, offset, segment count, and
  total remaining byte count.
- `p2p_iov_iter_count()` returns the total bytes not yet submitted.
- `p2p_iov_iter_addr()` returns the current physical address plus IOV offset.
- `p2p_iov_iter_single_seg_count()` clips work to the current physically
  contiguous IOV.
- `p2p_iov_iter_advance()` consumes submitted bytes and crosses IOV boundaries.

Keep the same three-stage shape as the current single-address path:

1. Initialize the destination iterator at the first exact PA segment.
2. Call `calc_read_size()` for each command. It returns the minimum of the
   current extent remainder, destination-segment remainder, queue maximum, and
   total transfer remainder.
3. After a successful submission, advance the destination iterator by the
   issued byte count. Move to the next segment only when the current segment is
   exhausted.

Keep source progress in the outer FIEMAP extent loop. Stop as soon as the IOV
transfer total has been submitted, even when the final source extent has an
unused tail. Report success when all requested destination bytes are consumed;
do not require exhaustion of every source extent or a next destination segment
after the exact final chunk.

This removes the existing final-entry overrun and naturally handles:

- one IOV contained within one device page;
- one IOV spanning several pages;
- several IOVs sharing the same size or having different sizes;
- an extent boundary inside an IOV;
- an IOV boundary inside an extent;
- queue-limit splits inside either one.

Preserve the current asynchronous ownership model: once the I/O context is
created, enqueue it on `batch->io_list` even if submission stops partway, store
the synchronous submission failure in `issue_err`, and let `drain_io` wait
for already-issued commands before freeing the context.

### 5. Simplify ioctl dispatch and cleanup

Use `IOCTL_READ_FILE` as the only read ioctl and call `p2p_read_file()` directly.
There must be no `is_batch` flag and no `iov_nr == 1` implementation fork below
request validation.

Use one cleanup ladder with explicit ownership transfer:

- The handler owns copied IOVs, extents, file reference, and PA IOVs while
  preparing the request.
- The I/O context owns the file reference and PA IOV allocation after
  construction.
- `free_io_ctx()` releases both exactly once after drain.

## Userspace Adaptation

Update `file_p2p/file_p2p_api.c` so it builds the new request through one
internal submit helper:

- A `read_parameter` describes a file name, starting file offset, and one or
  more destination IOVs.
- Sum the IOV lengths with overflow checks and use that total as the FIEMAP
  query length.
- Populate `host_pid`, `iov`, `iov_nr`, `ext_nr`, and the copied extents in
  `p2p_read_param`.
- `read_file()` submits one parameter with `IOCTL_READ_FILE`.
- `read_file_batch()` iterates its parameter array and submits each parameter
  with `IOCTL_READ_FILE`; all resulting contexts remain asynchronous on the
  same device fd and are collected by `drain_io()`.

Using several destination buffers for one contiguous file range no longer
requires `read_file_batch()`; callers put those buffers in one parameter's IOV
array. Update `py_file_p2p_api.c`, its method documentation, and `test.py` to
construct this contract and remove references to the deleted `bdev_name`,
`bdev_offset`, `addr`, and `size` fields.

## Validation

### Static and build checks

- `rg -n 'IOCTL_DUMP_PA|dump_pa|paddr_desc|read_desc|read_desc_ba|va_desc|va_desc_ba|get_pa_list_batch|new_io_ctx_batch|do_read_ios_batch'`
  returns no stale read-path users.
- `git diff --check`
- `make KSRC=/home/begin/code/oe_knl`
- `make -f test.mk` in `file_p2p/`
- `python setup.py build_ext --inplace` in `file_p2p/`

### Focused behavior checks

- Submit one aligned IOV and verify `read_file()` plus `drain_io()` succeeds.
- Submit one IOV with a valid page offset and a length smaller than one device
  page.
- Submit multiple unequal IOVs whose boundaries do not match FIEMAP extent
  boundaries.
- Submit extents whose total is larger than the IOV total and verify no command
  is issued for the unused extent tail.
- Submit a transfer larger than the queue maximum and verify it is split and
  drained successfully.
- Verify exact consumption of the last destination segment does not set
  `issue_err`.
- Reject zero counts, zero-length or unaligned entries, arithmetic overflow,
  insufficient extent coverage, and invalid userspace pointers without leaking
  a file reference, PA list, PA IOV array, command list, or I/O context.

## Non-Goals

- Changing the NVMe passthrough mechanism or tracepoint hook.
- Adding 32-bit compat-ioctl translation.
- Preserving the pre-`60ea660` binary UAPI layout.
- Changing drain batching, completion accounting, or global performance
  counters beyond what the merged context requires.
