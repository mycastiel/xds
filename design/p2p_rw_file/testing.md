# XDS write testing

## 1. Destructive test boundary

Write testing uses only disposable block devices in the prepared XDS VM.
The test environment rejects mounted devices, active swap, and block
devices with unexpected holders before allowing a raw block-device write.
Regular-file cases use only files on disposable VM filesystems. No
production device is a write target.

The existing CMB-backed physical-memory environment remains the source-memory
provider.  Every test source range and destination range is owned exclusively
for the duration of the operation.

## 2. Known-data pipeline

The basic write test uses one end-to-end data pipeline:

```text
deterministic host pattern
    -> host write to a raw seed range
    -> XDS read from the seed range into HBM
    -> drain and verify the read-side HBM CRC
    -> XDS write from the same HBM IOVs to a distinct raw target range
    -> drain
    -> uncached host read-back and exact comparison
```

The host retains the original deterministic pattern as the expected result for
both validation points.  The raw seed range and target range must not resolve to
the same physical blocks; otherwise an unchanged destination could make a
failed write appear successful.

### 2.1 Populate and validate HBM

The host prefills the raw seed range through an `O_DIRECT` block-device write.
The host buffer, seed offset, and transfer length satisfy the source device's
direct-I/O alignment requirements.  The prefill must complete the entire range;
a short write is a test failure.  Only after that completion does
`rw_file(P2P_IO_READ)` copy the seed range into the HBM IOVs, and `drain_io()`
establishes read completion.

Read-only CRC diagnostics calculate the CRC over the completed HBM destination.
The test compares that CRC with the CRC of the retained host pattern.  A
mismatch stops the case before write submission because the write source is not
known to be correct.

The HBM IOVs are not modified between successful read drain and write drain.
The write uses the same ordered IOV list that the read populated.  This avoids a
new CPU-to-HBM copy mechanism and tests the direction change of the unified
`rw_file()` API directly.

For a topology that consumes all disposable devices, the seed is loaded into
HBM before the target layout overwrites or repurposes those blocks.  The seed
and destination mappings remain physically distinct for the write being
verified.

### 2.2 Verify the block-device write

After the HBM CRC succeeds, the test submits `rw_file(P2P_IO_WRITE)` and waits
for `drain_io()` to complete.  It then opens the destination with `O_DIRECT`,
reads the complete verification window into an aligned host buffer, and compares
the result with the retained pattern and guard data.  The destination offset,
verification length, and host buffer satisfy the destination device's
direct-I/O alignment requirements.  A short read is a test failure; buffered
read-back and cache-invalidation fallbacks are not part of the verification
path.

Host read-back is the primary oracle because it is independent of the XDS read
path.  An XDS read-after-write round trip may provide additional mixed-direction
coverage, but it must not be the only correctness check.

Every destination case includes known guard data immediately before and after
the write range.  Read-back verifies both the requested bytes and the guards so
an otherwise correct payload cannot hide an offset or length overrun.

Kernel CRC and PA-content dumping are not used for writes.  The host read-back,
not an inspection of the HBM source, is the write-correctness oracle.

## 3. Device and boundary coverage

The write matrix covers the block-device forms already supported by topology
mapping:

- a direct NVMe namespace;
- an NVMe partition;
- dm-linear mappings, including component offsets and mapping boundaries; and
- MD RAID0 mappings, including chunk boundaries, member order, and partition
  members.

Raw-device cases remain unmounted. Separate filesystem phases exercise
regular-file writes on disposable filesystems.

Within each topology, deterministic cases cover:

- the minimum supported aligned transfer;
- a nonzero aligned device offset;
- multiple source IOVs whose boundaries do not match the device range splits;
- a source physical-page boundary;
- a queue-limit split;
- topology segment or RAID0 chunk boundaries; and
- the final aligned range ending at device capacity.

## 4. API and rejection coverage

Both the C and Python bindings use `rw_file()` for reads and writes.  The test
contract verifies that the operation field selects the direction and that the
same IOV and registered-memory interfaces work in both directions.

Negative cases are rejected before any NVMe command is submitted. Library
preallocation may already have changed file size or allocated blocks.  They
cover:

- an unknown operation value;
- a regular-file write through a read-only fd;
- a block-device file descriptor without write access;
- zero, unaligned, overflowing, or out-of-capacity ranges;
- insufficient extent coverage;
- unknown flags; and
- invalid, stale, foreign, or out-of-range registered-memory handles.

## 5. Drain, ordering, and lifetime coverage

Queued-write coverage submits several non-overlapping writes through one p2p fd
and drains them together.  Mixed coverage publishes independent reads and
writes on the same fd and verifies that one `drain_io()` waits for both kinds
of context.

A dependent read-after-write case uses an explicit drain between directions.
The test must not rely on submission order within one undrained mixed batch.

Normal and registered source-memory modes receive equivalent correctness
coverage.  Registered tests verify PA-list reuse and retain the registration
until every accepted write is drained.  Source memory remains unchanged until
drain returns and may be reused only afterward.

Failure injection covers partial request submission and hardware completion
failure.  Drain still waits for every issued child request, returns an error,
and releases the I/O context and memory ownership exactly once.  Closing a p2p
fd with undrained writes provides the corresponding release-path coverage.

## 6. Concurrency and observability

Concurrent stress uses disjoint destination ranges with deterministic patterns
so completion order cannot affect expected data.  Overlapping writes are not a
correctness case because their final contents are intentionally unordered.

After each phase, generic I/O accounting must show no inflight requests and the
expected issue or completion failures.  Write-only kernel logs must contain no
write CRC or PA-content dump.  Direction-tagged submission and error logs remain
available for diagnosing a failed case.

## 7. Regular-file write coverage

Cover allocation of empty files, nonzero offsets and growth, initialized
extent overwrites, unwritten readback, fragmented multiple extents, and
registered as well as one-shot source memory. Wait for write completion
before XDS readback. Also verify payload and guards through an independent
uncached read of the mapped blocks in the isolated VM. Ordinary pread of
unwritten extents may return zero and is not the payload oracle.

Local preparation tests cover allocation-before-FIEMAP ordering, allocation
failure propagation, read/bdev allocation bypass, signed offset overflow,
read-only fds, shared-write rejection, unsupported flags, and missing,
overlapping, unordered, or overflowing extent ranges. Kernel tests exercise
multi-extent write capacity and structural checks without allowing invalid
requests to reach NVMe submission. Batch tests distinguish preparation side
effects from accepted I/O and completion counts.
