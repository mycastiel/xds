# p2p_dev: Add debugfs I/O statistics

## 1. Objective and scope

Add a small debugfs interface for observing global NVMe request activity in
the `p2p_dev` module.  The interface reports request counts, submitted bytes,
in-flight requests, and failures separately for reads and writes across all
p2p device fds and all registered topologies.

The interface is diagnostic only.  It is not a stable userspace ABI and does
not change read, drain, topology, or completion semantics.

The debugfs directory and file are:

```text
/sys/kernel/debug/p2p_device/
└── summary
```

`summary` is created with mode `0600`.  Reading it returns one snapshot of the
statistics.  Any non-empty write resets the cumulative counters; the payload
is ignored.

## 2. Statistics and exact meanings

All counters are module-global per-CPU values (`DEFINE_PER_CPU`), following the
block layer `disk_stats` / `part_stat` pattern.  Each field in the per-CPU
object is a two-element array indexed by the internal `P2P_IO_STATS_READ`
(`0`) or `P2P_IO_STATS_WRITE` (`1`) macros.  The UAPI operation is explicitly
mapped to one of these indices inside the debugfs accounting helpers.  Hot-path
updates use `preempt_disable()` (and `local_t` for each `io_inflight`); readers
sum both indices across `for_each_possible_cpu`.

| Output field | Meaning | Resettable |
| --- | --- | --- |
| `{read,write}_io_issued` | Submitted NVMe requests by direction | yes |
| `{read,write}_io_bytes` | Submitted payload bytes by direction | yes |
| `{read,write}_io_inflight` | Requests awaiting completion by direction | no |
| `{read,write}_io_failed` | Failed completions by direction | yes |
| `{read,write}_io_issue_failed` | Issue-side failures by direction | yes |

Except for `{read,write}_io_issue_failed`, the unit is an individual lower
NVMe request, not an I/O ioctl, FIEMAP extent, I/O context, or drain operation.
Topology and queue-limit splitting therefore may cause one I/O ioctl to add
several `{read,write}_io_issued` operations.  The issue-failure fields are
counted once per I/O context whose request-generation loop returns an error.

`{read,write}_io_bytes` is submitted payload, calculated as
`sector_nr << SECTOR_SHIFT` at the successful issue point.  It does not claim
that a failed command transferred those bytes; the NVMe completion status does
not report a reliable partial byte count.

`{read,write}_io_issue_failed` is derived from `io_ctx->issue_err` immediately
after `do_ios()` returns.  It covers request-allocation failures, topology
mapping failures, zero-sized mapped segments, and source extents that do not
cover the complete destination.  A partial submission that issues some lower
requests before encountering one of these errors increments its direction's
issue-failure field once.  Validation, topology lookup, PA lookup, and other
errors that occur before the I/O context calls `do_ios()` are not counted.

## 3. Read format

Reading `summary` produces decimal values in this fixed order:

```text
read_io_issued: 123
read_io_bytes: 503808
read_io_inflight: 2
read_io_failed: 1
read_io_issue_failed: 4
write_io_issued: 17
write_io_bytes: 69632
write_io_inflight: 1
write_io_failed: 0
write_io_issue_failed: 0
```

The show callback sums each per-CPU counter into a local snapshot and then
formats the values with `seq_file`.  Each field is an approximate cross-CPU
sum, and the ten values are not a transaction: request submission,
completion, or reset may occur between the individual field sums.  This weakly
consistent view is sufficient for diagnostics and avoids a global lock in the
I/O hot path.

Use `single_open()`, `seq_read()`, `seq_lseek()`, and `single_release()` for the
read side of the file operations.

## 4. Reset semantics

The write callback does not parse or copy the payload.  Any write with a
nonzero byte count performs a reset and returns the supplied byte count.  A
zero-length write returns zero without changing the counters.

Reset zeroes both array elements for these cumulative counters on every
possible CPU:

- `io_issued`;
- `io_bytes`;
- `io_failed`; and
- `io_issue_failed`.

Each direction's `io_inflight` is a live gauge and is never reset.  Clearing
it while requests are active would make their later completions decrement the
gauge below zero.  Issue and complete may run on different CPUs, so one CPU's
`local_t` gauge may temporarily be negative; each direction's cross-CPU sum
remains its live count.  If reset races an increment, that event may fall on
either side of the reset.  If a request issued before reset completes with an
error after reset, its completion increments the new failure interval for
that direction.

Reset does not drain I/O, wait for completions, modify an I/O context, or alter
the per-fd batch list.

## 5. Accounting points and ordering

### 5.1 Successful issue

Instrument `do_io()` after the request and NVMe command are completely
initialized and after the I/O-context reference has been acquired, but before
calling `blk_execute_rq_nowait()`.

At that point, under `preempt_disable()`:

1. map `io_ctx->op` to the internal read or write array index;
2. increment this CPU's `io_issued` at that index;
3. add `sector_nr << SECTOR_SHIFT` to `io_bytes` at that index; and
4. `local_inc` `io_inflight` at that index.

The accounting must precede `blk_execute_rq_nowait()` because the request may
complete on another CPU before that function returns.  Accounting afterward
could let the completion decrement `io_inflight` before submission increments
it.

`blk_execute_rq_nowait()` has no synchronous error return.  Every request that
reaches it is counted as issued and must eventually run `p2p_end_io()`.

### 5.2 Issue failure

After `p2p_rw_file()` stores the result of `do_ios()` in `io_ctx->issue_err`,
increment the read or write `io_issue_failed` field once when that field is
nonzero.  Keep this accounting next to the assignment so it occurs even when
the I/O context contains successfully submitted requests that a later drain
must wait for.

Do not increment `io_issue_failed` directly in individual `do_io()` or mapping
error paths.  `do_ios()` stops at the first issue-side error, and
the single `io_ctx->issue_err` check provides exactly one count for the failed
I/O context.  Successfully submitted lower requests still contribute normally
to `io_issued`, `io_bytes`, and `io_inflight`.

### 5.3 Completion

At the start of `p2p_complete_io()`:

1. map `io_ctx->op` to the internal read or write array index;
2. increment `io_failed` at that index when `status` is nonzero; and
3. decrement `io_inflight` at that index exactly once for every completion.

This accounting is per request even though `io_ctx->io_err` retains only the
first error for the drain result.  It is independent of the existing
`io_ctx->io_ref` decrement and completion signaling.

The completion path must not take a mutex or spinlock.  Per-CPU updates under
`preempt_disable()` (with `local_t` for `io_inflight`) keep it safe in softirq
or other atomic completion contexts.

## 6. File organization and interfaces

Implement the debugfs object and counters in new files:

- `debugfs.c`: global statistics, debugfs file operations, reset, init, and
  exit;
- `debugfs.h`: accounting and lifecycle declarations used by `dev.c`; and
- `Makefile`: add `debugfs.o` to `p2p_dev-objs`.

The cross-file interface is conceptually:

```c
void p2p_debugfs_init(void);
void p2p_debugfs_exit(void);

#define P2P_IO_STATS_READ 0U
#define P2P_IO_STATS_WRITE 1U
#define P2P_IO_STATS_NR 2U

void p2p_stats_io_issued(u32 op, u64 bytes);
void p2p_stats_io_issue_failed(u32 op);
void p2p_stats_io_complete(u32 op, blk_status_t status);
```

The statistics object remains private to `debugfs.c`; `dev.c` does not access
individual counters directly.  This keeps the accounting contract centralized
and lets the debugfs presentation change without exposing storage details.

The counters use `DEFINE_PER_CPU` static storage (zero-initialized), so
accounting is valid before the debugfs directory is created and no
initialization step can erase an early request.

## 7. Initialization and teardown

Debugfs is optional diagnostics and must not determine whether `p2p_dev` can
load.

After topology and tracepoint initialization, `p2p_drv_init()` calls
`p2p_debugfs_init()` before `alloc_chrdev_region()`.  The resulting init order
is:

1. `topo_init()`;
2. `p2p_tp_hook_init()`;
3. `p2p_debugfs_init()`; and
4. character-device region allocation, cdev registration, class creation, and
   device creation.

Creating the interface before any character-device resource is allocated
keeps debugfs initialization independent of the cdev lifecycle and guarantees
that statistics are available before I/O becomes reachable.

`p2p_debugfs_init()`:

1. creates the top-level `p2p_device` directory;
2. creates its `summary`, `memory`, and `topo` files; and
3. records the root dentry for recursive removal.

An unavailable debugfs filesystem, including a kernel built without
`CONFIG_DEBUG_FS`, leaves the module operational without the directory.
Creation return values are intentionally ignored as recommended by the debugfs
API.  Child creation and recursive removal safely accept an error-valued root
dentry.

`p2p_drv_exit()` calls `p2p_debugfs_exit()` before removing the character
device or unregistering the NVMe tracepoint.  Exit calls
`debugfs_remove_recursive()` on the saved root and clears the pointer.  Removal
also excludes active debugfs callbacks before module text and statistics are
released.  The file operations set `.owner = THIS_MODULE`.

Although debugfs creation is nonfatal, the `alloc_chrdev_region()` failure and
every later module-init failure must pass through a new debugfs cleanup label.
That label calls `p2p_debugfs_exit()` before unregistering the tracepoint and
topology workqueue.  `p2p_debugfs_exit()` accepts a NULL root, so the unwind
path is identical when debugfs was unavailable.

## 8. Concurrency and overflow

- Submission, completion, summary reads, and resets may all run concurrently.
- No debugfs operation takes `batch->io_lock`, the topology lock, or a block
  queue lock.
- Hot-path updates are lockless across CPUs: each CPU mutates only its own
  counters under `preempt_disable()`.  Summary reads and resets sum or clear
  across CPUs without excluding I/O; values are weakly consistent.
- Counter overflow follows normal unsigned wraparound.  Saturating diagnostics
  would add hot-path branches without improving request safety.
- Concurrent drain behavior is unchanged.  A drain still snapshots only the
  I/O contexts already published on its batch list.
- Module unload is already prevented while character-device or debugfs file
  operations hold module references.

## 9. Error handling

- Debugfs directory or file creation failure: remove any partial tree, warn
  once, and continue loading the module.
- Any non-empty summary write resets the cumulative counters and returns its
  byte count; a zero-length write has no effect.
- Read formatting/allocation failure: propagate the normal `seq_file` error.
- Statistics never change the return value of submission, completion, drain,
  or module initialization.

## 10. Validation

### Functional checks

1. Load the module with debugfs mounted and verify `p2p_device/summary` exists
   with the expected mode and ten zero values.
2. Issue a direct-NVMe read and verify only the read group's `io_issued`,
   `io_bytes`, and `io_inflight` change at submission and completion.
3. Issue a direct-NVMe write and verify only the corresponding write fields
   change.
4. Issue linear and RAID0 reads that cross topology or queue boundaries and
   verify every generated lower request is counted separately.
5. Force `do_ios()` to return an issue-side error for each direction and verify
   its `io_issue_failed` field increments once for that I/O context.  In a
   partial submission case, verify already-issued lower requests remain
   reflected in the other counters.
6. Complete a request with a nonzero block status and verify `io_failed`
   increments while `io_inflight` returns toward zero.
7. Write an arbitrary non-empty payload with no I/O active and verify all
   cumulative counters become zero.
8. Reset while I/O is active and verify `io_inflight` is unchanged and reaches
   zero when those requests complete.
9. Race repeated reads and resets with submission and completion; values may be
   weakly consistent, but the file remains readable and the summed
   `io_inflight` does not become negative once all requests complete.
10. Unload and reload the module and verify the debugfs directory is removed
    and the counters restart at zero.

### Configuration checks

- Build and load with `CONFIG_DEBUG_FS=y`.
- Build with `CONFIG_DEBUG_FS` disabled and verify the module still loads with
  no debugfs directory.
- Run the kernel module build, userspace builds, `git diff --check`, and
  `checkpatch.pl` for the implementation patch.

## 11. Non-goals

- Per-fd, per-topology, per-namespace, or per-process statistics;
- latency histograms, bandwidth rates, or time-series storage;
- counting errors that occur before `do_ios()` as I/O issue failures;
- determining the actual byte count of a failed NVMe command;
- changing read/drain synchronization or waiting for I/O during reset; and
- providing a stable production monitoring ABI.
