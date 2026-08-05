# Completion queue, getevents, and submit (AIO / io_uring aligned)

Status: target design for the kernel **submit**, **completion**, and
**harvest** paths. The implemented NDS userspace contract remains
[fix3.md](fix3.md). This document supersedes the CQ / locking discussion
implied by the historical [completion.md](completion.md) for future kernel
work; it does not change the public `nds_io_submit` / `nds_io_getevents`
shapes, but it does define how those calls should map onto the kernel
(especially batch submit).

Related:

- Kernel drain lifetime: [../p2p_rw_file/drain.md](../p2p_rw_file/drain.md)
- Current UAPI: [`p2p_dev_uapi.h`](../../p2p_dev_uapi.h)
- Current publish / harvest: [`dev.c`](../../dev.c) (`p2p_publish_io_done`,
  `p2p_get_io_events`)
- Current NDS submit loop: [`file_p2p/nds_api.c`](../../file_p2p/nds_api.c)
  (`nds_io_submit`)

## 1. Goals

- Keep the application-visible I/O API **AIO-shaped**:
  `nds_io_submit` + `nds_io_getevents(min_nr, max_nr, timeout)` backed by
  kernel ioctls (`IOCTL_RW_FILE` / batch + `IOCTL_GET_IO_EVENTS`).
- Align **submit syscall boundaries** with `io_submit`: one kernel entry for
  a batch of accepted requests (fail-stop within the batch).
- Reduce lock contention between **I/O completion** (IRQ / softirq) and
  **getevents harvest** (process context).
- Shorten the IRQ-side critical section when publishing a completion.
- Preserve drain as full-fd quiesce only (`IOCTL_DRAIN_IO` for destroy /
  tests), not as the getevents engine.
- Keep completion records binary-compatible with `struct p2p_io_event` /
  `struct nds_io_event` (32 bytes: `user_data`, `res`, `reserved[2]`).

## 2. Non-goals

- Exposing a **mmap shared CQ** to applications in v1 (io_uring-style
  zero-syscall harvest). That would be a separate UAPI.
- Replacing NDS with `io_uring_enter` or requiring liburing.
- Changing `res` to a byte count (remains `0` on success or `-errno` per
  fix3; byte-count alignment with AIO/io_uring CQE is deferred).
- Making concurrent completions of different I/Os on one batch share **no**
  lock at all (CAS / sharded rings). That is a Phase-2 optimization after
  profiling.
- Guaranteeing completion order equal to submit order.
- Moving FIEMAP / extent packing into the kernel as a prerequisite for batch
  submit (userspace may still prepare extents per request; the **syscall**
  boundary is what must match AIO).

## 3. Linux AIO model

Linux AIO (`fs/aio.c`) uses a per-`kioctx` **completion ring** shared with
userspace (via the aio ring mapping). Conceptually:

```text
aio_complete (IRQ / softirq path)
  spin_lock(completion_lock)
    write io_event into ring[tail]
    smp_wmb()
    advance ring->tail
  spin_unlock(completion_lock)
  wake waiters if needed

io_getevents (process)
  mutex_lock(ring_lock)
    observe head / tail
    copy events to caller
    advance ring->head
  mutex_unlock(ring_lock)
```

Properties that matter for XDS:

| Property | AIO behavior |
| --- | --- |
| Complete vs getevents | Different locks (`completion_lock` vs `ring_lock`); they do **not** share one spinlock on the hot path |
| Concurrent completes on one ctx | Still serialize on `completion_lock` |
| Wait semantics | `min_nr`, relative timeout, indefinite wait; timeout returns available count (may be 0); interrupt keeps harvested events when any exist |
| Event identity | `user_data` / obj cookie echoed into `io_event` |
| `res` | Bytes transferred or `-errno` |

AIO **does not** eliminate producer–producer contention among completions on
the same context. It primarily splits **producer vs consumer**.

### 3.1 AIO submit (`io_submit`)

```text
io_submit(ctx, nr, iocbpp)
  one syscall
  kernel walks iocb[0..nr) from the head
  on first failure: stop (fail-stop within the batch)
  return accepted count, or -errno if none accepted
  accepted work completes later into the ring → io_getevents
```

Accept ≠ complete. Bytes / errors appear only in completion events.

## 4. io_uring model

io_uring uses a pair of rings (SQ + CQ) typically **mmap’d** into the
application:

```text
Kernel (producer on CQ)
  fill CQE at slot
  memory barrier
  advance CQ tail

Application (consumer on CQ)
  load CQ tail (acquire)
  read CQE(s)
  advance CQ head (release)
  often without a syscall if events are already posted

Wait for completions
  io_uring_enter(... IORING_ENTER_GETEVENTS ...)
  or poll the shared tail
```

Properties:

| Property | io_uring behavior |
| --- | --- |
| Harvest without syscall | Possible when CQ already has events (shared memory) |
| Wait | Folded into `io_uring_enter`, or busy-poll |
| `cqe.res` | Syscall-equivalent result: bytes or `-errno` |
| `user_data` | Echoed from SQE |
| Multi-producer publish | Kernel still orders CQ slot publication; overflow is tracked |

Relative to AIO: io_uring goes further on **userspace harvest cost** (mmap CQ).
Relative to lock splitting: the idea of a **ring with head/tail** is the same
family as AIO; XDS can adopt the ring without adopting mmap.

### 4.1 io_uring submit

```text
application fills one or more SQEs, advances SQ tail
io_uring_enter(...)   # often one enter for the batch; SQPOLL may avoid enter
kernel consumes SQ → issues work
each SQE produces one CQE (user_data echoed)
```

Batching is native to the SQ ring. It is **not** “N independent ioctls in a
userspace loop.” Submit and completion remain split (SQE vs CQE), like AIO’s
accept vs getevents.

## 5. Current XDS status

Today each `p2p_batch` (one `/dev/p2p_device` fd) uses:

- `io_list` — submitted, not yet fully retired to harvestable state
- `done_list` — completed, waiting for getevents / drain
- a **single** `spinlock_t io_lock` covering both lists and `io_cnt` /
  `done_cnt`
- `wait_queue_head_t wait` for getevents

Completion publish (`p2p_publish_io_done`) takes `io_lock`, moves the context
onto `done_list`, wakes waiters, and may call `complete(&io_done)` **while
still holding `io_lock`**.

Getevents (`p2p_get_io_events`) waits until enough completions are ready,
harvests under the **same** `io_lock`, then fills a **kernel**
`kvmalloc_array` of `p2p_io_event` and `copy_to_user`s to the caller.

Userspace NDS already passes a caller-owned `nds_io_event[]` that is layout-
identical to `p2p_io_event` (zero-copy at the library boundary). The extra
kernel intermediate buffer is still present.

```text
Public today:
  nds_io_submit     → N × IOCTL_RW_FILE (userspace loop; see §8)
  nds_io_getevents  → IOCTL_GET_IO_EVENTS
  destroy           → IOCTL_DRAIN_IO then close

Kernel today (change on completion path):
  complete ──io_lock──► done_list
  getevents ──io_lock──► harvest + kvmalloc + copy_to_user
```

NDS submit already uses fail-stop within the batch (return accepted count or
`-errno`), matching AIO’s return convention, but each accepted iocb still
pays a separate `IOCTL_RW_FILE`.

## 6. Chosen design (completion / getevents)

Adopt an **AIO-style split**: a per-batch **completion ring** plus separate
locks for publish and harvest. Keep the **ioctl getevents** surface (do not
mmap the CQ to applications in v1). Borrow io_uring’s ring head/tail mental
model and barriers; do not require applications to speak CQ rings.

```text
NVMe complete
    │
    ▼
publish to CQ ring          ◄── completion_lock only
    │  fill slot, advance tail, wake
    │  complete(io_done) OUTSIDE lock
    ▼
IOCTL_GET_IO_EVENTS
    │  wait until (tail - head) >= min_nr or timeout / empty inflight
    ▼
harvest                       ◄── ring_lock / harvest path (not completion_lock)
    │  copy slots to user one-by-one (__copy_to_user)
    │  advance head, free_io_ctx
    ▼
user nds_io_event[]
```

### 6.1 Completion ring

- One ring per `p2p_batch`, fixed capacity (power-of-two preferred; bound to
  the batch’s maximum outstanding I/O).
- Each slot carries enough state to emit one `p2p_io_event` and to retire the
  corresponding `p2p_io_context` after a successful copy to userspace (either
  store the event fields in the slot and keep an `io_ctx` pointer, or store
  the `io_ctx` and fill the event at harvest time — implementation choice as
  long as publish stays short).
- Ring indices: kernel producer advances **tail** under `completion_lock`;
  harvest advances **head** under the harvest lock. Visibility uses the usual
  `smp_wmb` before publishing tail and acquire-style loads for consumers.

### 6.2 Locks

| Lock | Held by | Protects |
| --- | --- | --- |
| `completion_lock` | Completion / publish path | CQ slot write, `tail`, wake decision inputs owned by producer |
| Harvest lock (`ring_lock` or equivalent) | `IOCTL_GET_IO_EVENTS` | `head`, consuming slots, free-after-copy |
| Inflight accounting | Submit / retire (process) | Accepted CQ slot count (`io_cnt`); see [fix4.md](fix4.md) Change A to drop redundant `io_lock`/`io_list` |

Getevents **must not** take `completion_lock` to copy events out. Completions
**must not** take the harvest lock to publish.

`complete(&io_ctx->io_done)` moves **out of** `completion_lock`. Drain and
getevents share `ring_lock` (they are not used concurrently by the NDS API;
the mutex serializes them if they ever overlap). Drain waits for natural
publish then harvests the CQ — it does **not** steal in-flight contexts or
suppress publish, so there is no `on_batch` flag.

### 6.3 Getevents behavior (locked semantics)

Matches current UAPI and AIO-shaped wait rules:

| Input | Behavior |
| --- | --- |
| `timeout_ns < 0` | Wait until at least `min_nr` harvestable completions, or no remaining work that can still complete (`ready_events == outstanding_events`) |
| `timeout_ns == 0` | Non-blocking harvest up to `max_nr` |
| `timeout_ns > 0` | Relative timeout; return how many were harvested (may be 0); not an error |
| `-EINTR` / restart | If any events already harvested, return that count; else return `-EINTR` |
| `res` | `0` on success, `-errno` on failure (fix3) |
| Order | CQ publication / completion order; not submit order |
| `min_nr > 0`, no outstanding | Returns immediately with `0` events (does not block like AIO on an empty ctx); see [fix4.md](fix4.md) |

Harvest copies **directly** into the user `events` array with per-event
`copy_to_user` *before* consuming (advancing the cursor / head). On fault or
when `max_nr` stops mid-slot, the consumer-owned cursor (`cq_head_consumed`)
simply stays where it is and the next call resumes — no requeue path needed.

With merged submissions (§8.4) one CQ slot expands into `nr_ios` events;
`min_nr` / `max_nr` and the wait condition are counted in logical I/Os via
`ready_events` / `outstanding_events`, not CQ slots.

### 6.4 Public API boundary

Applications continue to use:

```c
int nds_io_getevents(struct nds_io_ctx *ctx, int min_nr, int nr,
		     struct nds_io_event *events, struct timespec *timeout);
```

They do **not** map a CQ, call `io_uring_*`, or call `drain_io` for normal
completion. Drain remains destroy / full quiesce only.

## 7. Phased implementation (completion)

| Phase | Change | Notes |
| --- | --- | --- |
| **0** | Drop the getevents `kvmalloc_array` intermediate; copy events out one-by-one | Improves getevents without changing list structure; safe first step |
| **1** | Introduce CQ ring; split `completion_lock` vs harvest; move `complete()` off the publish lock | Primary contention fix (complete ↔ getevents) |
| **2** (future) | Profile-driven: CAS / sharded publish to reduce complete ↔ complete contention; optional mmap CQ UAPI | Beyond AIO mainline; separate design if mmap is desired |

Batch submit (§8) is independent and may land before, with, or after Phase 0/1.

## 8. Submit path: AIO, io_uring, and XDS

### 8.1 AIO `io_submit`

- One system call submits up to `nr` iocbs.
- Kernel fail-stop: process from the head of the array; on the first error,
  stop. Return the number already accepted, or a negative errno if none were
  accepted.
- Accepted work is not yet complete; completions appear later on the ring and
  are harvested with `io_getevents`.

### 8.2 io_uring submit

- The application posts one or more SQEs and advances the SQ tail, then
  typically issues one `io_uring_enter` (fewer enters with SQPOLL).
- Each SQE maps to one CQE; `user_data` is echoed.
- Batching is a first-class ring property, not a userspace loop of N
  independent submission syscalls.

### 8.3 XDS today

[`nds_io_submit`](../../file_p2p/nds_api.c) walks `iocb[0..nr)` in userspace:

```text
for each iocb:
  validate / FIEMAP / build p2p_io_param   # may calloc
  ioctl(IOCTL_RW_FILE)                     # one syscall per iocb
  free param
  on failure: return i (accepted) or -errno
return nr
```

| Aspect | Aligned with AIO? |
| --- | --- |
| Fail-stop return counts | Yes |
| Accept ≠ complete | Yes |
| One syscall per batch | **No** — N × `IOCTL_RW_FILE` |
| Per-request userspace alloc / FIEMAP | XDS-specific cost (extent prep) |

### 8.4 XDS target (locked)

Public API stays:

```c
int nds_io_submit(struct nds_io_ctx *ctx, int nr,
		  const struct nds_io_cb *iocb);
```

Kernel / UAPI target — no batch ioctl. `IOCTL_RW_FILE` already carries
`iov[iov_nr]` / `extents[ext_nr]` arrays, so a batch is expressed as one
merged request plus a sub-I/O table that preserves the per-iocb boundaries:

1. `struct p2p_io_param` gains `sub_ios` (pointer to
   `struct p2p_sub_io[nr_ios]`) and `nr_ios`. Each `p2p_sub_io` is
   `{ user_data, iov_cnt, ext_cnt }`: one logical I/O owning the next
   `iov_cnt` iovs and `ext_cnt` extents of the merged arrays.
   `nr_ios == 0` keeps today's single-I/O semantics (`file_p2p` unchanged).
2. The kernel accepts the merged request as **one** `io_ctx` / one CQ slot,
   but counts and completes it as `nr_ios` logical I/Os:
   - submit: `outstanding_events += nr_ios`;
   - publish: one CQ slot, `ready_events += nr_ios`;
   - `getevents` waits on `ready_events` (so `min_nr`/`max_nr` are counted
     in logical I/Os) and expands the slot into `nr_ios` events carrying the
     per-sub `user_data`, resuming mid-slot when `max_nr` stops early.
   Sub-I/O extent slices are consumed independently, so a FIEMAP-oversized
   trailing extent of one iocb cannot leak into the next iocb's buffers.
3. NDS merges consecutive iocbs sharing `(op, flags, file_fd, host_pid,
   mem_handle)` into one `IOCTL_RW_FILE`, fail-stop across groups, and
   returns the accepted iocb count or `-errno` if none accepted.
4. Every accepted logical I/O is still harvested with
   `IOCTL_GET_IO_EVENTS` — orthogonal to §6.

```text
nds_io_submit(nr)
  prepare param[0..nr) in userspace
  merge consecutive mergeable iocbs → few (typically one) IOCTL_RW_FILE,
  each carrying iov[] + extents[] + sub_ios[]
  return accepted count or -errno
        │
        ▼
   accepted logical I/Os → CQ (this doc §6)
        │
        ▼
nds_io_getevents (events counted per logical I/O)
```

## 9. Comparison summary

### 9.1 Completion / getevents

| | AIO | io_uring | XDS target (this doc) |
| --- | --- | --- | --- |
| App harvest API | `io_getevents` | mmap CQ + optional `enter` | `IOCTL_GET_IO_EVENTS` / `nds_io_getevents` |
| Shared mmap CQ | Yes (aio ring) | Yes | **No** in v1 |
| Complete vs harvest locks | Split | Ring + barriers; kernel sync on publish | Split (AIO-like) |
| Complete vs complete | Shared `completion_lock` | Kernel-ordered CQ publish | Shared `completion_lock` until Phase 2 |
| `res` | Bytes / `-errno` | Bytes / `-errno` | `0` / `-errno` (fix3) |
| Drain / cancel all | ctx destroy | ring teardown | `IOCTL_DRAIN_IO` |

### 9.2 Submit

| | AIO | io_uring | XDS today | XDS target |
| --- | --- | --- | --- | --- |
| Submit API | `io_submit` one syscall | SQ + `io_uring_enter` | N × `IOCTL_RW_FILE` | one merged `IOCTL_RW_FILE` (`sub_ios[]`) |
| Fail-stop within batch | yes (kernel) | per-SQE / ring rules | yes (userspace loop) | group all-or-nothing, fail-stop across groups |
| Accept vs complete | split | SQE vs CQE | split | split |
| Extent / FIEMAP prep | N/A (page cache path) | N/A | userspace per iocb | userspace per iocb OK |

## 10. Acceptance criteria for an implementation of this design

1. Completions and getevents no longer share one spinlock for publish vs
   harvest.
2. Publish-side critical section does not call `complete()` while holding
   `completion_lock`.
3. Getevents does not allocate a temporary kernel array of events for the
   normal path (Phase 0+).
4. Public NDS / UAPI getevents semantics above remain unchanged.
5. Drain still retires all published work and leaves the module unloadable
   after destroy/exit in existing tests.
6. Existing NDS and `file_p2p` basic/stress suites still pass.
7. For `nds_io_submit` with `nr > 1` mergeable iocbs on the success path, the
   library enters the kernel **once** via a merged `IOCTL_RW_FILE` carrying a
   `sub_ios[]` table (not N times). Partial failure still returns an accepted
   count or a negative errno consistent with `io_submit`.
