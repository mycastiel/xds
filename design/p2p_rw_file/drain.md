# Read/write drain, ordering, and buffer lifetime

## 1. One mixed batch

Each p2p fd owns one batch containing both read and write I/O contexts.
`rw_file()` validates, submits, and publishes a context without waiting for
hardware completion.  `drain_io()` atomically snapshots the contexts already
published on that fd and waits for every read and write in the snapshot.

An `rw_file()` call or another drain that overlaps the snapshot need not be
covered by that drain.  Closing the p2p fd drains every remaining published
context before releasing its resources.

## 2. Completion and error contract

A successful `rw_file()` return means that the logical operation was accepted;
it does not mean that hardware I/O has completed.  A context remains drainable
when request submission fails after some child requests were issued.  Drain
waits for those children, releases all contexts in its snapshot, and returns
the first logical error only after the complete snapshot has been processed.

The same submission-error, completion-error, reference-counting, and cleanup
rules apply to both directions.

## 3. Buffer lifetime and synchronization

The operation determines the caller's buffer obligation:

- after submitting a read, the caller must not consume or reuse destination
  memory until drain returns;
- before submitting a write, the caller must finish producing the source data;
  it must not modify, release, or reuse that memory until drain returns; and
- XDS retains the one-shot pin or registered-memory I/O reference through the
  complete drain-time access interval.

XDS does not synchronize an accelerator producer implicitly.  Producer
completion is a precondition of submitting a write.

## 4. Ordering and durability

Reads and writes within one undrained batch may execute concurrently.  Their
submission order is not a data-dependency guarantee.  A caller that requires a
read to observe a preceding write drains the write before submitting the read.

Drain guarantees NVMe request completion and permits buffer reuse.  It does
not by itself guarantee power-loss persistence.  Persistence through FUA or an
explicit cache flush is outside the initial `rw_file()` contract.

## 5. Diagnostics

Submission, completion, byte, inflight, and failure accounting applies to both
directions.  Logs identify the operation so a mixed drain remains auditable.

Optional CRC and PA-content dumping apply only to successful reads.  Drain
must skip both operations for every write context.  Write correctness is
verified by a separate host block-device read-back as defined by the testing
design, not by inspecting the write source buffer.
