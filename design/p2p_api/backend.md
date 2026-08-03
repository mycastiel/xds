# NDS backend abstraction

> **Status:** Historical proposal. The authoritative implemented v1 contract
> is [fix3](fix3.md); conflicting backend and helper descriptions below are
> superseded.

This document is part of the [p2p_api](../p2p_api.md) design.  It must not be
implemented in isolation from the [UAPI](uapi.md), [completion](completion.md),
[objects](objects.md), or [testing](testing.md) contracts.

## 1. Goals

NDS keeps one public API and pluggable backends.  v1 installs a backend that
only calls existing `file_p2p` helpers and `/dev/p2p_device` ioctls.  A future
io_uring backend may replace the transport without changing `nds_io_cb` or
`nds_io_event`.

The backend layer is also where **code style and reliability** constraints from
the entry document are enforced for implementers.

## 2. Backend vtable

```c
struct nds_backend_ops {
	int (*init)(struct nds_init_param *param);
	int (*shutdown)(void);
	int (*register_mem)(void *addr, __u64 size, int flags, __u64 *mem_handle);
	int (*unregister_mem)(void *addr);
	int (*new_ctx)(const struct nds_io_ctx_param *param,
		       struct nds_io_ctx **ctx);
	int (*destroy_ctx)(struct nds_io_ctx *ctx);
	int (*submit)(struct nds_io_ctx *ctx, unsigned int nr,
		      struct nds_io_cb *iocb);
	int (*getevents)(struct nds_io_ctx *ctx, unsigned int min_nr,
			 unsigned int nr, struct nds_io_event *events,
			 struct timespec *timeout);
};
```

`nds_init` installs exactly one backend for the process (v1: ioctl backend).
Switching backends after init is out of scope for v1 (`-EBUSY` if re-init is
attempted with a different backend selection mechanism later).

## 3. v1 ioctl / file_p2p mapping

| Backend op | Existing helper / ioctl | Notes |
| --- | --- | --- |
| `init` | open bookkeeping `new_p2p_fd()`; init addr→handle map | process-wide |
| `shutdown` | unregister remaining map entries; close bookkeeping fd | after ctxes destroyed or force-cleaned |
| `register_mem` | `register_mem` / `IOCTL_REGISTER_MEM` | out `*mem_handle`; map insert |
| `unregister_mem` | map lookup by base `addr` → `unregister_mem` | GDS-shaped key |
| `new_ctx` | `new_p2p_fd`; alloc inflight + CQ of `max_io_nr` | one p2p fd per ctx |
| `destroy_ctx` | getevents until empty and/or `drain_io` full quiesce; `close_p2p_fd` | drain OK for destroy |
| `submit` | validate → resolve obj → auto `add_topo` → `rw_file` (+ `user_data` echo) | dual memory modes |
| `getevents` | `IOCTL_GET_IO_EVENTS` → fill `nds_io_event[]` | **not** full-fd drain |

### 3.1 Registered submit translation

```text
nds_io_cb with NDS_IO_F_REGISTERED_MEM
  → io_parameter.flags = P2P_IO_F_REGISTERED_MEM
  → io_parameter.mem_handle = iocb.mem_handle
  → io_parameter.host_pid = 0
  → iov.addr = buf_addr, iov.size = buf_len
  → rw_file(ctx->p2p_fd, &io_parameter)
```

### 3.2 One-shot submit translation

```text
nds_io_cb with rw_flags clear and mem_handle == 0
  → io_parameter.flags = 0
  → io_parameter.mem_handle = 0
  → io_parameter.host_pid = iocb.obj.host_pid   /* 0 = current process */
  → iov.addr = buf_addr, iov.size = buf_len
  → rw_file(ctx->p2p_fd, &io_parameter)
```

Path/fd resolution and FIEMAP/synthetic extents remain inside `rw_file` or a
thin NDS open-for-PATH helper that still ends in the same ioctl packing.  Do
not duplicate topology or extent logic in the backend.

## 4. Style checklist (match file_p2p)

Implementers must:

1. Return negative errno; avoid inventing NDS-specific status enums.
2. Reject unknown flags and nonzero pads before calling `rw_file`.
3. Use early returns; keep functions small and ioctl wrappers thin.
4. Log failures with `fprintf(stderr, "... errno: %d\n", err)` patterns
   consistent with `file_p2p_api.c` where diagnostics help VM tests.
5. Reuse `add_topo`, `register_mem`, `rw_file`, and completion ioctls
   (`IOCTL_GET_IO_EVENTS` for getevents; `drain_io` only for full quiesce)
   rather than opening `/dev/p2p_device` with a parallel private protocol.
6. Keep Python bindings (if any) as thin as `py_file_p2p_api.c`.

## 5. Reliability checklist

1. **Fd isolation**: never share one p2p fd across two NDS contexts; never
   issue non-NDS `rw_file` on an NDS ctx fd.
2. **Accept ≠ complete**: submit only publishes; getevents
   (`IOCTL_GET_IO_EVENTS`) owns completion visibility for apps.
3. **Mode honesty**: no silent promotion from one-shot to registered based on
   address lookup.
4. **Teardown**: `destroy_ctx` must not leak published contexts; `shutdown`
   must not leave registered memory or open fds behind after success.
5. **Topo errors**: cache successful `add_topo` per bdev identity; surface
   failures at submit with the underlying errno.
6. **Capacity**: when inflight is full, return `-EAGAIN` (or the count of
   already-accepted requests in the batch) per the UAPI submit rules.

## 6. Future io_uring backend

A future backend may:

- drive the same `nds_io_cb` / `nds_io_event` shapes;
- keep registered vs one-shot flag rules unchanged;
- use io_uring only internally.

It must not:

- require applications to include `liburing` headers or call `io_uring_*`;
- change `user_data` echo or `res` errno conventions;
- relax the one-logical-completion-per-accepted-submit rule.

Faithful getevents for the ioctl backend requires `IOCTL_GET_IO_EVENTS` as
specified in [completion.md](completion.md) §5.  Full-fd `drain_io` is not a
substitute for timed/`min_nr` getevents; it remains for destroy and
low-level quiesce only.

## 7. Library layout (suggested)

```text
nds_api.h          public header
nds_api.c          dispatch to backend ops
nds_backend_ioctl.c v1 file_p2p mapping
```

or colocated under `file_p2p/nds_*.c` linking the existing objects.  Either
layout is acceptable if the public symbol set and style rules hold.

## 8. Non-goals

- Multiple concurrent backends in one process in v1.
- Rewriting topology discovery or FIEMAP inside NDS.
- Changing topology / register ioctl meaning beyond the additive completion
  UAPI in [completion.md](completion.md) / [uapi.md](uapi.md).
- Implementing NDS getevents solely with `IOCTL_DRAIN_IO`.
