# NDS completion model

> **Status:** Historical proposal. The authoritative implemented v1 contract
> is [fix3](fix3.md); conflicting completion descriptions below are
> superseded.

This document is part of the [p2p_api](../p2p_api.md) design.  It must not be
implemented in isolation from the [UAPI](uapi.md), [backend](backend.md),
[objects](objects.md), or [testing](testing.md) contracts.

It extends the kernel/userspace drain rules in
[p2p_rw_file/drain.md](../p2p_rw_file/drain.md) into a GDS-like submit/getevents
surface.  Applications see getevents; they do not call `drain_io`.

## 1. Goals

- Preserve XDS async accept semantics: submit publishes work; completion is
  separate.
- Present completions as discrete `nds_io_event` records with echoed
  `user_data` and a zero-or-errno `res`.
- Honor aio/GDS-shaped **`min_nr` / `max_nr` / timeout`** on getevents; do
  not pretend fd-wide `IOCTL_DRAIN_IO` already provides that (see §5).
- Keep buffer lifetime rules as strong as the existing drain contract.
- Keep `IOCTL_DRAIN_IO` for full quiesce (destroy / low-level tests), not as
  the long-term getevents engine.

## 2. Context queues

Each `nds_io_ctx` holds:

- one dedicated `/dev/p2p_device` fd;
- an inflight table of capacity `max_io_nr`;
- a completion queue (CQ) of the same capacity.

```text
nds_io_submit
    → validate / translate / rw_file (echo user_data / io cookie)
    → slot moves to inflight (accepted)

Target path:
  IOCTL_GET_IO_EVENTS(min_nr, max_nr, timeout)
    → kernel returns up to max_nr newly harvested completions
    → library fills nds_io_event[] (user_data, res)

Transitional / destroy-only:
  IOCTL_DRAIN_IO
    → wait for ALL published on this fd
    → library may split into CQ (over-waits; no real timeout)
```

Because drain (and getevents on one fd) only covers work published on that
fd, the library must not interleave unrelated `rw_file` traffic on an NDS
ctx fd.  One NDS ctx ↔ one p2p fd is mandatory for reliability.

## 3. Submit batch rules

`nds_io_submit(ctx, nr, iocb)` processes requests from the head of `iocb`:

1. If the next request cannot be accepted (validation error, topo error,
   `rw_file` failure, or inflight full), stop.
2. If at least one prior request in this call was accepted, return that count
   (non-negative).
3. If none were accepted, return the negative errno of the failing request
   (or `-EAGAIN` when inflight is full at the first slot).

This fail-stop-within-batch rule is intentional for v1 simplicity and matches
a predictable GDS-like batch behavior.  Already-accepted requests remain
inflight and must be harvested with getevents.

A successful accept does **not** mean NVMe completion. Completion success is
reported as zero in `nds_io_event.res`.

## 4. getevents and timeouts

```c
int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout);
```

- `nr` is the maximum events to copy out; `min_nr` is the minimum to wait for
  before returning under a blocking wait (`min_nr == 0` means non-blocking
  harvest of whatever is already complete, after optionally progressing the
  fd once).
- `timeout == NULL` waits indefinitely for `min_nr`.
- `timeout` with zero or expired interval returns the number of events already
  available (possibly `0`) without treating timeout as an error.
- Each event echoes `user_data` and `opcode` from the accepted iocb.
- `res == 0` means the logical request completed successfully;
  `res < 0` is a negated errno for that request after children have finished.

Error aggregation follows the spirit of drain: hardware children are waited;
the event carries the logical result once the request is fully retired.

## 5. Relationship to drain_io — and why a new kernel API is needed

### 5.1 Semantic mismatch

`nds_io_getevents(min_nr, nr, timeout)` is an **aio / GDS-style** completion
API.  Today's `IOCTL_DRAIN_IO` / `drain_io()` is **not** that API:

| Capability | `nds_io_getevents` | `drain_io` today |
| --- | --- | --- |
| Wait until **at least** `min_nr` requests finish | required | no — waits for **every** context already published on the fd |
| Return **at most** `nr` events, leave the rest pending | required | no — one call retires the whole snapshot |
| **Timeout** while waiting for `min_nr` | required | no — blocks until the snapshot is fully done |
| Per-request `user_data` / `res` to userspace | required | drain returns one aggregated errno for the snapshot |

So: **full-fd drain alone does not implement getevents.**  Mapping getevents
onto repeated `drain_io` calls is at best an approximation with serious
limitations (section 5.2).  A production-faithful NDS completion path needs a
**new kernel completion interface** (section 5.3).  The public NDS surface
stays submit/getevents either way; only the kernel↔library transport changes.

### 5.2 What a drain-only userspace shim can and cannot do

With one NDS ctx ↔ one p2p fd, a library *can*:

- call `drain_io` when it wants to **progress** work;
- split the drained logical I/Os into a userspace CQ keyed by `user_data`;
- on getevents, copy **at most** `nr` events from that CQ (max-count is a
  pure userspace concern once events are already complete).

It **cannot** faithfully provide:

1. **`min_nr` without over-waiting** — `drain_io` does not wake when the N-th
   of M inflight requests completes; it wakes only when the entire published
   snapshot is done.  A getevents(`min_nr=1`) with 16 inflight still blocks in
   the kernel until all 16 finish if the library uses one drain to progress.
2. **`timeout` during wait** — there is no way to bound an in-flight
   `IOCTL_DRAIN_IO` wait with `struct timespec`.  Timing out in userspace only
   works *before* entering drain, or by abandoning the wait (not supported).
3. **Partial kernel retirement** — drain retires the whole snapshot; the
   library cannot ask the kernel to “complete any one request and return.”

**Transitional use of drain** is still acceptable for:

- `nds_io_destroy_ctx` / `nds_shutdown` (must wait for **all** remaining I/O);
- early bring-up sync helpers when the caller submits one request and waits
  for that single completion (drain of a one-request fd matches getevents(1));
- tests that only need “wait until quiet,” not aio-shaped polling.

It is **not** acceptable as the final story for GDS-compatible
`nds_io_getevents` under concurrent multi-inflight load.

### 5.3 New kernel completion UAPI (required for faithful getevents)

Add a completion ioctl that mirrors aio `io_getevents` / GDS batch get-status
semantics on the same p2p fd batch.  Sketch (exact names/layout belong in a
follow-on UAPI patch; this locks intent):

```c
struct p2p_io_event {
	__u64 user_data;   /* correlated at submit time — see below */
	__s64 res;         /* 0 on success or -errno */
	__u32 op;          /* read/write echo optional */
	__u32 flags;
};

struct p2p_getevents_param {
	__u32 min_nr;
	__u32 max_nr;
	__s64 timeout_ns;  /* <0 wait forever; 0 nonblock; >0 relative timeout */
	struct p2p_io_event *events; /* userspace array, length max_nr */
};

#define IOCTL_GET_IO_EVENTS _IOWR('k', /* next */, struct p2p_getevents_param)
```

**Required kernel behavior:**

1. Wait until at least `min_nr` **already published** I/O contexts on this fd
   have completed, or until `timeout_ns` expires (return the number completed
   so far, possibly 0 — not an error).
2. Return up to `max_nr` events; leave additional completed contexts available
   for a later call (or keep them completed-but-unharvested until harvested).
3. Each event identifies one logical `rw_file` acceptance and carries a
   stable correlation token so NDS can fill `nds_io_event.user_data`.

**Correlation token:** today's `IOCTL_RW_FILE` does not return a cookie.
The completion ioctl therefore needs either:

- an in/out cookie field added to `p2p_io_param` at submit time (preferred:
  userspace passes `user_data`, kernel echoes it on completion), or
- a kernel-assigned id returned from submit that NDS maps to `user_data`.

Prefer extending the submit path with an echoed `__u64 user_data` (or opaque
`io_id`) so getevents does not invent a second naming scheme.  That is a
**small, additive** UAPI change, still separate from inventing a second drain.

**Drain remains:** `IOCTL_DRAIN_IO` stays for “wait for everything published
on this fd and tear down,” used by destroy, low-level tests, and unregister
races.  It is not replaced; it is no longer the engine behind NDS getevents.

### 5.4 Phased implementation

| Phase | Kernel | NDS getevents behavior |
| --- | --- | --- |
| **v1-bringup** (optional) | unchanged | drain-backed shim; document over-wait / no real timeout; only safe for low inflight or single-ops sync helpers |
| **v1-complete** (design target) | `IOCTL_GET_IO_EVENTS` (+ submit echo cookie) | faithful `min_nr` / `max_nr` / timeout; drain used only for destroy / full quiesce |

Shipping NDS as GDS-aligned without the new ioctl means advertising getevents
semantics the kernel cannot yet honor.  The entry design’s earlier “no kernel
UAPI change” applies to **not reshaping** existing ADD_TOPO / RW_FILE /
REGISTER_MEM layouts casually; it does **not** forbid an additive completion
ioctl required by the public NDS contract.  Update [backend.md](backend.md) and
the entry [p2p_api.md](../p2p_api.md) acceptance criteria accordingly when
implementing: faithful getevents depends on `IOCTL_GET_IO_EVENTS`.

### 5.5 Summary

```text
Public:   nds_io_getevents(min_nr, max_nr, timeout)   ← must stay
Today:    IOCTL_DRAIN_IO  = wait ALL published on fd   ← insufficient alone
Needed:   IOCTL_GET_IO_EVENTS (+ submit user_data echo)
Keep:     IOCTL_DRAIN_IO for destroy / full quiesce
```

Because kernel drain snapshots **all** published contexts on the fd when used,
the library must still not interleave unrelated `rw_file` traffic on an NDS
ctx fd.  One NDS ctx ↔ one p2p fd remains mandatory.

## 6. Buffer lifetime

Aligned with [drain.md](../p2p_rw_file/drain.md):

- After a successful submit of a read, the caller must not consume or reuse
  destination HBM until the matching event is returned.
- Before submit of a write, the caller must finish producing source HBM; it
  must not modify or free that memory until the matching event is returned.
- Registered and one-shot pins remain held through retirement, as today.
  Harvest via getevents (or destroy-time drain) is what releases that
  obligation to the caller.

Synchronous `nds_io_pread` / `nds_io_pwrite` do not create a second lifetime
rule; they wait for the single event internally.

## 7. Ordering

Accepted requests on one ctx may complete out of order relative to submit
order.  Callers that need read-after-write on the same ranges must wait for
the write's event before submitting the dependent read (same rule as mixed
batches under drain).

## 8. Destroy and shutdown

`nds_io_destroy_ctx`:

1. Progress outstanding inflight with getevents until empty, or call
   `drain_io` once to quiesce the fd (destroy may wait for **all** remaining
   work — full drain is appropriate here);
2. discard any remaining CQ entries only after results are no longer needed
   (destroy implies the caller abandons unread events — prefer draining so
   kernel pins are released cleanly);
3. close the p2p fd;
4. free ctx memory.

`nds_shutdown` requires that no ctx remain, or it destroys remaining ctxes
with the same drain discipline before releasing process-wide registrations.

## 9. Reliability notes

- Never report an event for a request that was not accepted by submit.
- Never lose an accepted request across destroy without waiting kernel
  completion (pin/ref leaks).
- Map library-level failures after partial child issue into a single `res`
  after wait, consistent with existing I/O context error handling.
- Do not claim timeout or `min_nr` semantics in docs/tests unless
  `IOCTL_GET_IO_EVENTS` (or equivalent) backs them.

## 10. Non-goals

- Exporting `drain_io` on the NDS surface as a substitute for getevents.
- Guaranteeing submit-order completion.
- Power-loss durability beyond what NVMe completion already provides.
- Cross-ctx event polling in one call.
- Using full-fd drain as the long-term implementation of timed/`min_nr`
  getevents.
