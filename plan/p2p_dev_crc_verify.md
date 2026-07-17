# p2p_dev: CRC32 Verification for Read Data

## Summary

Add an optional diagnostic that calculates the CRC32 of data written by a P2P
read after all NVMe requests for that read have completed.  The kernel logs one
checksum for each successfully drained `p2p_io_context`; a standalone userspace
tool calculates the same checksum over the corresponding source-file range.
Matching values verify that the bytes placed in the destination PA IOVs are the
same as the bytes read through the filesystem interface.

This is an opt-in debug feature guarded by `CALC_CRC32`.  It is not enabled in
normal module builds because CPU-reading all P2P destination memory adds a full
extra pass over the data and serializes that work in `IOCTL_DRAIN_IO`.

## Current State

The current read path already retains everything needed for CRC calculation:

- `struct p2p_io_context` pins the source `struct file` until drain cleanup;
- `pa_iov` contains the exact destination physical address and length segments
  in logical read order, with first- and last-page padding removed;
- `data_size` is the sum of the requested destination IOV lengths; and
- `p2p_drain_io()` calls `wait_io_done()` before freeing the context.

Therefore, do not add a second list of PAs used during request submission and
do not copy the dentry name into the context.  Reuse `io_ctx->pa_iov` for the
CRC byte stream and print the name through the already pinned `io_ctx->file`
with `%pD`.  This keeps checksum metadata consistent with the same PA IOVs that
drive `do_read_ios()` and avoids another lifetime/allocation path.

The checksum log does not need the logical file offset.  The test invocation
already identifies the requested offset, while `io_ctx->data_size` supplies the
length.  Do not add an offset field to `p2p_io_context` or change the read UAPI.

`dump_pa_content()` only maps the beginning of `pa_iov[0]`; it is not suitable
for verification because one read can span many PA IOVs.

## CRC Contract

Use the conventional reflected CRC-32 value implemented by kernel
`crc32_le()`:

```text
polynomial:  0xedb88320
initial CRC: 0xffffffff
final XOR:   0xffffffff
```

Initialize a running value with `~0U`, pass the returned raw value from one PA
IOV to the next, and apply `^ ~0U` exactly once after the final byte.  The
checksum covers exactly `io_ctx->data_size` bytes by concatenating every
`pa_iov` segment in array order.  PA gaps, alignment padding, unused extent
tails, and bytes beyond `data_size` are excluded.

Both implementations must produce these known values:

- an empty input: `00000000`;
- `123456789`: `cbf43926`.

The feature is CRC32, not CRC32C.

## Kernel Design

### 1. Compile-time gating

Place the CRC-only include and helpers under `#ifdef CALC_CRC32`.  Enable the
diagnostic for a build with, for example:

```sh
make W=1 KCFLAGS="-Werror -DCALC_CRC32" KSRC=/home/begin/code/oe_knl
```

The selected kernel must provide `CONFIG_CRC32`; `crc32_le()` is exported by
the local kernel.  A build without `CALC_CRC32` must preserve the current read,
drain, logging, and module-dependency behavior.

### 2. Calculate CRC over the destination PA IOVs

Add a void CRC helper that accepts one completed `p2p_io_context`, calculates
the checksum, and logs the result.

For each `pa_iov` in order:

1. Map the exact range with `ioremap(pa_iov.addr, pa_iov.len)`.
2. Pass the returned mapped VA directly to `crc32_le()` with the running CRC as
   its seed.
3. Call `iounmap()` before advancing to the next PA IOV.

The target platform permits direct CPU reads from this `ioremap()` mapping, so
do not allocate a scratch buffer and do not copy the data through
`memcpy_fromio()`.  Keep the mapping typed as `void __iomem *` and use an
explicit `__force` cast only at the `crc32_le()` call, whose parameter is an
ordinary byte pointer.

If `ioremap()` fails, log the failure and return from the helper without a CRC
result.  The failure is diagnostic-only and does not change the drain result.
Every successful mapping must be unmapped before advancing to the next PA IOV.

### 3. Drain integration and errors

Keep checksum calculation in `p2p_drain_io()` immediately after
`wait_io_done()` has reported success and before `free_io_ctx()`:

1. Preserve the existing `issue_err` and block-completion error handling.
2. Skip CRC calculation for any context with a submission or completion error;
   its destination may contain only partial data.
3. After successful completion, use `dma_rmb()` to order the subsequent CPU
   reads after DMA completion, then calculate the CRC over the PA IOVs.  This
   barrier is ordering, not a replacement for provider-specific cache
   maintenance on non-coherent memory.
4. Log the result only when the complete CRC calculation succeeds.  A mapping
   failure is logged by the helper but does not become a drain error.

CRC calculation does not change request completion accounting and must not run
from `end_read_io()`, which is an atomic completion callback and may represent
only one of several requests belonging to the context.

Use one stable, machine-searchable log format:

```text
p2p: crc32 file=<dentry-name> length=0x<hex> crc32=<8-hex-digits>
```

One log line represents the ordered concatenation of all destination IOVs in
that `p2p_io_context`, not one line per NVMe command, PA segment, or userspace
IOV.

## Userspace Reference Tool

Add a standalone C program at `test/crc32_verify.c`.  It must not depend on the
Python extension or on a P2P device.  Its interface is:

```text
crc32_verify -f <file-or-block-device> [-o <offset>] [-l <length>]
```

`-f` is required.  The default offset is zero.  When `-l` is omitted, calculate
from the selected offset through the end of the regular file or block device;
with the default offset, this is the whole object.  Determine a regular-file
size with `fstat()` and a block-device size with `BLKGETSIZE64`, then reject an
offset beyond that size or an explicitly requested range that exceeds it.

The tool must:

- use `getopt()` and reject unknown options, missing arguments, malformed
  numbers, and extra positional arguments;
- parse offset and length as bounded nonnegative integers, accepting decimal
  or `0x` notation;
- open the path read-only and use a bounded buffer plus `pread()` until exactly
  the selected length has been consumed from the selected offset;
- retry interrupted reads and reject an unexpected EOF or `off_t` overflow;
- implement the CRC contract above without requiring an external library; and
- print the file, effective offset, effective length, and zero-padded
  eight-digit CRC so the CRC can be compared directly with the kernel log.

The implementation may build a 256-entry table for polynomial `0xedb88320` at
startup.  Buffer boundaries must not reset or finalize the running CRC.

## Build Integration

Update the top-level `Makefile` with a phony `test` target that builds
`test/crc32_verify` from `test/crc32_verify.c` with warnings enabled.  The
target only builds the test utility; it does not run a P2P test.  Keep the
existing default module build unchanged, and extend `clean` to remove the test
binary without removing its source or directory.

`file_p2p/test.mk` is not part of this feature.

## Verification Workflow

1. Build and load `p2p_dev.ko` with `CALC_CRC32` enabled.
2. Run `make test` to build `test/crc32_verify`.
3. Submit a P2P read and do not modify or reuse any destination IOV until its
   covering drain returns.
4. Call `IOCTL_DRAIN_IO` and collect the kernel checksum line.
5. Run `test/crc32_verify -f <file> -o <offset> -l <iov-byte-count>` for the
   same stable source range.
6. Compare the two eight-digit CRC values.

The source file must not change between extent preparation, P2P I/O, and the
reference read.  Dirty page-cache data must be flushed before the test because
the P2P path reads the mapped block sectors while the reference tool reads the
logical file.  Destination memory must be CPU-readable through `ioremap()`;
the current PA-query interface provides no alternate copy or cache-maintenance
operation for memory that is not CPU-accessible or coherent.

This design intentionally leaves comparison to the test workflow.  The kernel
does not receive an expected CRC, so a checksum mismatch is not returned as an
ioctl error.

## Files Affected During Implementation

- `dev.c`: gated CRC helper, drain calculation, and result/error logging.
- `test/crc32_verify.c`: standalone source-range CRC calculator.
- `Makefile`: `test` build target and test-binary cleanup.

`p2p_dev_uapi.h`, `file_p2p/file_p2p_api.c`, the Python binding, and
`file_p2p/test.mk` do not need to change for this design.

## Acceptance Criteria

- A normal build without `CALC_CRC32` behaves as it does before this feature.
- An enabled build computes CRC only after every submitted request in the
  context has completed successfully.
- Multi-IOV and multi-PA reads produce one CRC over the exact logical
  destination byte sequence, including PA IOV boundary cases.
- Submission/completion failures produce no misleading checksum line.
- Mapping failures produce no checksum line and do not leak mappings, file
  references, or I/O contexts; they do not change the drain result.
- The userspace tool accepts the required `-f` and optional `-o`/`-l` interface,
  including the default whole-object range.
- Kernel and userspace CRC helpers pass the empty-input and `123456789`
  vectors.  End-to-end checks agree for single-segment, multi-segment, offset,
  default-range, and batch-read test cases.
- `git diff --check` passes, all inline review markers are resolved, the module
  builds both with and without `CALC_CRC32`, and `make test` builds the
  userspace utility with warnings enabled under `test/`.
