# fix5 — iocb-boundary submit (no cross-iocb merge)

Status: implement the corrected NDS submit model. Depends on
[`completion_getevents.md`](completion_getevents.md) and the lock cleanup in
[`fix4.md`](fix4.md).

## Problem

An earlier direction treated `nds_io_submit(nr, iocb)` as “merge same-file
iocbs into one `IOCTL_RW_FILE` with `sub_ios[]`”. That mismatches the API
boundary:

| Layer | Meaning | Ioctls |
| --- | --- | --- |
| One `nds_io_cb` | One `obj.fd`, one `offset`, many `iov` (SG), one `user_data` | **One** `IOCTL_RW_FILE` with `nr_ios=0` |
| `nds_io_submit(nr)` | Independent control blocks; often **different** files | **N** ioctls, one per iocb |

Cross-iocb merge, same-file API enforcement, and batch FIEMAP are **out of
scope / rejected**.

## Submit semantics (AIO fail-stop)

`nds_io_submit` loops iocbs in order:

| Case | Return |
| --- | --- |
| All accepted | `nr` |
| First fails / nothing accepted | `-errno` |
| First `k` accepted, then one fails (`0 < k < nr`) | `k` (positive; **no** errno for the failed iocb) |
| `nr == 0` | `0` |

Accepted I/Os are not rolled back and complete normally via getevents.

## Kernel counters / locks (audit only)

- Keep `ready_events` / `outstanding_events`: they count **logical** events;
  `cq_tail - cq_head` / `io_cnt` count **CQ slots**. Needed while UAPI allows
  one slot → `nr_subs` events.
- Keep `completion_lock` / `ring_lock`. Do not move `ready_events` bump outside
  `completion_lock` after `cq_tail` (drain / non-blocking getevents harvest on
  `cq_tail` and would race).
- Micro-opt in `p2p_get_io_events`: one `atomic_sub(total, …)` at unlock
  instead of per-event `atomic_dec` (single consumer under `ring_lock`).

## UAPI `sub_ios` / `nr_ios`

Remain in the kernel for optional merged submissions. **NDS does not use
cross-iocb merge**; each iocb uses `nr_ios=0`.

## Checklist

- [x] This file (`fix5.md`).
- [x] Simplify [`nds_api.c`](../../file_p2p/nds_api.c): delete merge helpers;
      one ioctl per iocb + fail-stop.
- [x] `p2p_get_io_events` batch `atomic_sub(total)`.
- [x] Sync [`nds_api.h`](../../file_p2p/nds_api.h) and
      [`completion_getevents.md`](completion_getevents.md).
- [x] Tests / KASAN smoke.
