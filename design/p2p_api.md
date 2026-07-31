# XDS portable P2P API (NDS)

## 1. Status and scope

This document is the entry point for a GDS-aligned userspace API on top of
XDS.  The public surface uses the `nds_*` prefix (NDS).  The design theme name
is `p2p_api`.

Today XDS exposes a low-level C/Python helper API (`file_p2p_*`) and kernel
ioctls on `/dev/p2p_device`.  That surface is sufficient for topology-aware
HBM↔NVMe P2P I/O, but it does not match cuFile/GDS call shapes that frameworks
(for example Mooncake) already integrate against, and it does not leave a
clean place for alternate completion backends such as io_uring.

This design covers:

- a stable GDS-like userspace contract (`nds_init`, memory registration, I/O
  context, submit/getevents, and thin synchronous helpers);
- **both HBM input modes already supported by XDS**, as first-class NDS
  submit options (not inferred only from buffer addresses):
  1. **Registered**: caller registers HBM once, then passes an opaque
     `mem_handle` plus VA coordinates within that range; the kernel reuses the
     cached PA list.
  2. **One-shot**: caller passes HBM `VA + size` on each I/O; the kernel calls
     `devmm_get_mem_pa_list()` for that request and builds NVMe commands from
     the returned PAs.
- completed structure definitions that were missing from the draft comparison
  (`nds_init_param`, opcodes, object types, `nds_io_event`, `mem_handle` on
  the I/O control block);
- a v1 backend that maps onto existing `file_p2p` helpers plus an **additive**
  kernel completion ioctl (`IOCTL_GET_IO_EVENTS` and submit `user_data` echo)
  so getevents can honor `min_nr` / `max_nr` / timeout; existing ADD_TOPO /
  REGISTER_MEM / RW extent packing stay compatible;
- automatic topology registration as a library concern (no GDS counterpart);
- an explicit evolution path where submit/getevents remain the public shape
  and io_uring is only a future backend.

This version does **not**:

- reshape non-completion ioctl layouts casually, or replace `IOCTL_DRAIN_IO`
  (drain remains for destroy / full quiesce only);
- pretend full-fd `drain_io` implements timed/`min_nr` getevents;
- provide `cuFileHandleRegister` / `cuFileHandleDeregister` equivalents
  (explicitly NA; each I/O carries an `nds_io_obj`);
- implement a real io_uring backend;
- implement KV or object-store backends (types/opcodes are reserved and return
  `-EOPNOTSUPP` in v1).

`file_p2p_*` remains available as a lower-level / transitional API used by
tests and by the NDS v1 backend.

### 1.1 Implementation style, reliability, and usability

NDS v1 is a facade, not a second I/O stack.  Implementation and review must
follow the habits already established in `file_p2p/file_p2p_api.c` and the
kernel module:

**Code style**

- Return negative Linux errno on failure; succeed with `0` or a documented
  non-negative count/byte result (same convention as `file_p2p`).
- Validate early and reject unknown flag bits, nonzero reserved fields, and
  inconsistent registered/one-shot combinations before any ioctl submit.
- Keep wrappers thin: translate `nds_*` into existing helpers (`new_p2p_fd`,
  `add_topo`, `register_mem`, `rw_file`) and the completion ioctl
  (`IOCTL_GET_IO_EVENTS`); use `drain_io` only for full quiesce/destroy —
  not as the getevents engine.  Do not re-implement FIEMAP, topology
  discovery, or NVMe command construction.
- Prefer stderr diagnostics on failure paths in the style of `file_p2p` (useful
  in prepared-VM tests); do not invent a parallel logging subsystem.
- Do not reshape ADD_TOPO / REGISTER_MEM / core RW packing beyond the additive
  completion cookie and `IOCTL_GET_IO_EVENTS` (see [completion.md](p2p_api/completion.md)).

**Reliability**

- One NDS I/O context owns exactly one `/dev/p2p_device` fd so internal
  `drain_io()` cannot retire unrelated work.
- A successful `nds_io_submit` means acceptance only; buffers stay owned by the
  caller until the matching `nds_io_event` is returned from `nds_io_getevents`.
- Registered and one-shot modes are explicit and mutually exclusive on each
  `nds_io_cb`; the library must not infer mode solely from address ranges.
- `nds_io_destroy_ctx` and `nds_shutdown` must progress or drain outstanding
  I/O before releasing fds, registrations, and topology cache entries.
- Topology auto-registration failures must surface as submit-time errno, not
  silent fallback.

**Availability / usability**

- Keep a GDS-shaped surface so frameworks can integrate without learning the
  low-level `file_p2p` ioctl packing.
- Both HBM modes remain first-class and usable from sync helpers
  (`flags` / `mem_handle` on `nds_io_pread` / `nds_io_pwrite`).
- Synchronous I/O is only submit(1)+getevents(1); applications never call
  `drain_io` or io_uring for NDS completions.  Faithful getevents requires
  `IOCTL_GET_IO_EVENTS` (drain alone is insufficient — see completion design).
- Errors remain ordinary errno values already understood by existing XDS tests
  and callers.

The [backend](p2p_api/backend.md) and [testing](p2p_api/testing.md) sub-designs
expand these rules into concrete mapping and regression checklists.

## 2. Sub-design documents

- [UAPI and userspace contract](p2p_api/uapi.md) defines the public header,
  GDS↔NDS mapping, registered vs one-shot memory flags, error codes,
  validation rules, and reserved fields.
- [Backend abstraction](p2p_api/backend.md) defines the ctx/backend vtable,
  the v1 ioctl/`file_p2p` mapping, style/reliability checklist, and the
  io_uring backend slot.
- [Completion model](p2p_api/completion.md) defines submit queueing,
  `nds_io_getevents` vs `drain_io`, why `IOCTL_GET_IO_EVENTS` is required,
  timeouts, and buffer lifetime.
- [I/O objects and topologies](p2p_api/objects.md) defines `nds_io_obj`
  semantics for file/bdev (and future KV/obj), path vs fd rules, and
  automatic `add_topo` caching.
- [Testing](p2p_api/testing.md) requires existing basic/stress to keep
  passing and adds parallel NDS basic/stress suites.

These documents form one design together with this entry.  A sub-design must
not be implemented in isolation if doing so violates the public contract,
completion rules, dual memory-mode rules, style/reliability constraints, or
backend ownership rules in another.

## 3. Chosen design

### 3.1 Layering

```text
Application / Mooncake
        │
        ▼
   nds_* public API          ← GDS-aligned, stable
        │
        ▼
   nds backend (vtable)
        │
   ┌────┴────┐
   ▼         ╎
 v1 ioctl    ╎ future io_uring backend
 file_p2p    ╎
   │
   ▼
 /dev/p2p_device  (p2p_dev.ko)
```

Applications must not call io_uring APIs for NDS I/O.  Future backends may use
io_uring internally without changing `nds_io_cb` or `nds_io_event`.

### 3.2 GDS alignment

| Purpose | GDS API | NDS API |
| --- | --- | --- |
| Initialize library | `cuFileDriverOpen` | `nds_init` |
| Shut down library | `cuFileDriverClose` | `nds_shutdown` |
| Register file handle | `cuFileHandleRegister` | **NA** (use `nds_io_obj` per I/O) |
| Deregister file handle | `cuFileHandleDeregister` | **NA** |
| Register device memory | `cuFileBufRegister` | `nds_register_mem` |
| Deregister device memory | `cuFileBufDeregister` | `nds_unregister_mem` |
| Create batch / I/O context | `cuFileBatchIOSetup` | `nds_io_new_ctx` |
| Destroy batch / I/O context | `cuFileBatchIODestroy` | `nds_io_destroy_ctx` |
| Submit async I/O | `cuFileBatchIOSubmit` | `nds_io_submit` |
| Harvest completions | `cuFileBatchIOGetStatus` | `nds_io_getevents` |
| Synchronous pread/pwrite | `cuFileRead` / `cuFileWrite` | `nds_io_pread` / `nds_io_pwrite` (submit + getevents) |

XDS-only concerns that have no GDS counterpart (topology discovery, CMB stub
parameters, debugfs) stay inside the library or lower-level APIs and are not
part of the primary GDS comparison table.

### 3.3 Public structures

```c
#define NDS_API_VERSION 1u

struct nds_init_param {
	__u32 version;     /* must be NDS_API_VERSION */
	__u32 flags;       /* v1: must be 0; unknown bits → -EOPNOTSUPP */
	__u64 reserved[4]; /* must be zero */
};

struct nds_io_ctx_param {
	__u32 max_io_nr;   /* capacity of inflight + completion queue */
	__u32 flags;       /* v1: must be 0 */
	__u64 pad;
};

struct nds_io_ctx {
	union {
		__s32 fd;  /* v1: owning /dev/p2p_device fd, or -1 if unused */
		__u64 id;  /* alternate backend cookie */
	};
	/* implementation may embed additional private state via returned pointer;
	 * the public handle type is an opaque pointer or this small struct —
	 * see uapi sub-design.  v1 treats nds_io_ctx * as an opaque library object
	 * whose first field matches this layout for debugging. */
};

#define NDS_IO_OBJ_FD   1u  /* .fd holds an open file or block fd */
#define NDS_IO_OBJ_PATH 2u  /* .str holds a NUL-terminated path */
#define NDS_IO_OBJ_ID   3u  /* future KV/obj key; v1 → -EOPNOTSUPP */

struct nds_io_obj {
	__u32 type;
	__s32 host_pid;   /* 0 = current process; must be 0 if REGISTERED_MEM */
	union {
		__u64 id;
		const char *str;
		__s32 fd;
	};
	__u64 pad2;
};

#define NDS_IO_OP_PREAD    0u
#define NDS_IO_OP_PWRITE   1u
#define NDS_IO_OP_PREADV   2u
#define NDS_IO_OP_PWRITEV  3u

/* Memory mode for nds_io_cb.rw_flags — mirrors P2P_IO_F_REGISTERED_MEM. */
#define NDS_IO_F_REGISTERED_MEM (1u << 0)
#define NDS_IO_F_MASK           NDS_IO_F_REGISTERED_MEM

struct nds_io_cb {
	__u32 opcode;
	__u32 rw_flags;       /* NDS_IO_F_REGISTERED_MEM or 0; unknown bits → -EOPNOTSUPP */
	struct nds_io_obj obj;
	__u64 offset;         /* byte offset into file/bdev */
	__u64 user_data;      /* echoed in nds_io_event */
	__u64 buf_addr;       /* HBM VA, or pointer to iovec array for *V */
	__u32 buf_len;        /* byte length, or iovec count for *V */
	__u32 pad1;
	__u64 mem_handle;     /* required iff REGISTERED_MEM; else must be 0 */
	__u64 pad2;
};

struct nds_io_event {
	__u64 user_data;      /* from iocb */
	__s64 res;            /* >=0 bytes transferred; <0 negated errno */
	__u32 opcode;         /* echoed opcode */
	__u32 flags;          /* v1: 0 */
	__u64 reserved[2];
};
```

Notes:

- For `PREAD`/`PWRITE`, `buf_addr`/`buf_len` describe one contiguous HBM
  range.  For `PREADV`/`PWRITEV`, `buf_addr` points to an array of `{addr,len}`
  pairs and `buf_len` is the count (v1 may implement *V by translating to the
  existing multi-`p2p_iov` path).
- Write to a regular file remains `-EOPNOTSUPP` (same rule as `rw_file`).
- Memory mode is **explicit** on each I/O via `rw_flags` / `mem_handle` (see
  section 3.4).  The library must **not** silently infer registered vs
  one-shot only from whether `buf_addr` falls in a previously registered
  range; that would hide which kernel path is used.

### 3.4 Two HBM memory modes (both required in v1)

XDS already has two kernel paths.  NDS v1 exposes both; neither is optional.

| Mode | Caller supplies | Kernel behavior | When to use |
| --- | --- | --- | --- |
| **Registered** | Prior `nds_register_mem` → `mem_handle`; I/O sets `NDS_IO_F_REGISTERED_MEM`, nonzero `mem_handle`, and `buf_addr` as a VA **within** the registered range | Lookup cached PA list; no per-I/O `devmm_get_mem_pa_list` | Hot path / GDS-like BufRegister reuse |
| **One-shot** | `rw_flags` clear, `mem_handle == 0`, `buf_addr` + `buf_len` = HBM VA + size | Per-I/O `devmm_get_mem_pa_list` / `put`; build NVMe SGL from those PAs | Ad-hoc transfers; tests; buffers not worth registering |

Validation (aligned with existing `IOCTL_RW_FILE`):

- Registered: `NDS_IO_F_REGISTERED_MEM` set ⇒ `mem_handle != 0`; every
  buffer VA must lie in that registration; library sets
  `P2P_IO_F_REGISTERED_MEM` and passes the handle; `host_pid` must be 0.
- One-shot: flag clear ⇒ `mem_handle` must be 0; `host_pid` identifies the
  process owning the HBM VAs (`0` = current process, same as `rw_file`);
  kernel pins VA+size for this I/O only.
- Mixing a nonzero handle with a clear flag, or a zero handle with the flag
  set, is `-EINVAL` before submit.

`nds_register_mem` **returns the opaque handle to the caller** (out-parameter).
`nds_unregister_mem` may still accept the registration **base address** for
GDS-shaped teardown; the library maps `addr → mem_handle`.  An explicit
`nds_unregister_mem_handle(mem_handle)` may be added as a thin alias; it is
not required for v1 if address-based unregister remains.

```text
Registered:
  nds_register_mem(va, size, 0, &handle)
  iocb.rw_flags = NDS_IO_F_REGISTERED_MEM
  iocb.mem_handle = handle
  iocb.buf_addr = va + displacement   # still the numeric HBM VA
  iocb.buf_len = len
  nds_io_submit(...)

One-shot:
  iocb.rw_flags = 0
  iocb.mem_handle = 0
  iocb.buf_addr = hbm_va
  iocb.buf_len = size
  nds_io_submit(...)
```

### 3.5 Public functions

```c
int nds_init(struct nds_init_param *param);
int nds_shutdown(void);

/* On success, writes the opaque kernel mem_handle to *mem_handle. */
int nds_register_mem(void *addr, __u64 size, int flags, __u64 *mem_handle);
int nds_unregister_mem(void *addr);

int nds_io_new_ctx(const struct nds_io_ctx_param *param, struct nds_io_ctx **ctx);
int nds_io_destroy_ctx(struct nds_io_ctx *ctx);

int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr,
		  struct nds_io_cb *iocb);
int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout);

/*
 * Synchronous helpers: submit(1) + getevents(min_nr=1).
 * flags/mem_handle follow the same registered vs one-shot rules as nds_io_cb.
 */
ssize_t nds_io_pread(struct nds_io_ctx *ctx, struct nds_io_obj *obj,
		     void *buf, size_t len, off_t offset,
		     __u32 flags, __u64 mem_handle);
ssize_t nds_io_pwrite(struct nds_io_ctx *ctx, struct nds_io_obj *obj,
		      const void *buf, size_t len, off_t offset,
		      __u32 flags, __u64 mem_handle);
```

Return convention: `0` or a non-negative byte/count result on success;
negative Linux errno on failure, unless a function documents otherwise
(`nds_io_getevents` returns the number of events harvested, or a negative
errno).

### 3.6 Mapping onto existing XDS

| NDS | Existing XDS |
| --- | --- |
| `nds_init` | Process-wide library state; open a private bookkeeping p2p fd for registrations if needed |
| `nds_shutdown` | Unregister remaining library-owned state; close bookkeeping fds |
| `nds_register_mem` | `register_mem` / `IOCTL_REGISTER_MEM`; return `mem_handle`; optional addr→handle map for unregister-by-addr |
| `nds_unregister_mem` | Lookup by `addr` → `unregister_mem` / `IOCTL_UNREGISTER_MEM` |
| `nds_io_new_ctx` | `new_p2p_fd`; allocate inflight table and completion queue of `max_io_nr` |
| `nds_io_destroy_ctx` | Quiesce remaining I/O (`drain_io` OK here); `close_p2p_fd` |
| `nds_io_submit` (registered) | `rw_file` with `P2P_IO_F_REGISTERED_MEM` + `mem_handle` + IOV VAs (+ `user_data` echo) |
| `nds_io_submit` (one-shot) | `rw_file` with flags clear, `host_pid` from `iocb.obj` (`0` → current), IOV = HBM VA+size (+ `user_data` echo) |
| `nds_io_getevents` | `IOCTL_GET_IO_EVENTS` (`min_nr` / `max_nr` / timeout); **not** full-fd `drain_io` |
| `nds_io_pread`/`pwrite` | Build one `nds_io_cb` (including flags/handle) → submit(1) → getevents(1) → return `res` |

### 3.7 Completion model (v1)

The public completion API is **getevents**, not `drain_io`.  Full-fd drain
cannot express `min_nr` or timeout; additive `IOCTL_GET_IO_EVENTS` (plus
submit `user_data` echo) is required for faithful semantics — see
[completion.md](p2p_api/completion.md) §5.  Drain remains for destroy / full
quiesce only.

- Each successfully accepted `nds_io_cb` occupies one inflight slot and carries
  `user_data` until completion.
- `nds_io_submit` returns how many requests were accepted from the head of the
  array (or a negative errno if none were accepted).  A request that fails
  validation is not enqueued; later requests in the same submit call are not
  processed after the first failure (fail-stop within the batch).
- One NDS ctx ↔ one p2p fd so completion harvest cannot mix unrelated work.
- `nds_io_getevents` waits for at least `min_nr` (or timeout), returns at most
  `nr` events, and leaves further completions for later calls.
- Buffer ownership: after a successful submit, the caller must not reuse or
  free HBM covered by that request until the corresponding event is returned.

Synchronous helpers introduce **no second completion semantic**: they only
compose submit and getevents.

### 3.8 Objects, topologies, and multi-semantic I/O

v1 supported object types:

| `type` | Meaning | Read | Write |
| --- | --- | --- | --- |
| `NDS_IO_OBJ_FD` | Open regular file or block fd | file or bdev | bdev only |
| `NDS_IO_OBJ_PATH` | Path string; library opens for the request | file or bdev | bdev only |
| `NDS_IO_OBJ_ID` | Future KV/obj | `-EOPNOTSUPP` | `-EOPNOTSUPP` |

On first use of a block device (by path or fd identity), the library calls
`add_topo` and caches success per ctx (or process-wide).  Topology failures
surface as submit-time errno.  An optional explicit `nds_add_topo` may appear
in the objects sub-design as an XDS extension; it is not part of the GDS
comparison table.

### 3.9 Backend evolution and io_uring

```c
struct nds_backend_ops {
	int (*init)(struct nds_init_param *param);
	int (*shutdown)(void);
	int (*register_mem)(void *addr, __u64 size, int flags, __u64 *mem_handle);
	int (*unregister_mem)(void *addr);
	int (*new_ctx)(const struct nds_io_ctx_param *param, struct nds_io_ctx **ctx);
	int (*destroy_ctx)(struct nds_io_ctx *ctx);
	int (*submit)(struct nds_io_ctx *ctx, unsigned int nr, struct nds_io_cb *iocb);
	int (*getevents)(struct nds_io_ctx *ctx, unsigned int min_nr, unsigned int nr,
			 struct nds_io_event *events, struct timespec *timeout);
};
```

v1 installs the ioctl/`file_p2p` backend.  A future io_uring backend must
preserve `nds_io_cb` / `nds_io_event` (including both memory modes) and the
submit/getevents ordering rules.  Whether that backend uses io_uring
underneath, or the kernel later grows uring-friendly ioctls, is an
implementation choice that must not leak into the public header.

### 3.10 Pseudocode

```c
int nds_init(struct nds_init_param *param)
{
	if (!param || param->version != NDS_API_VERSION)
		return -EINVAL;
	if (param->flags || nonzero_reserved(param->reserved))
		return -EOPNOTSUPP;
	/* install v1 backend; open bookkeeping p2p fd; init addr→handle map */
	return backend->init(param);
}

int nds_register_mem(void *addr, __u64 size, int flags, __u64 *mem_handle)
{
	struct p2p_mem_register_param p = { .addr = (u64)addr, .size = size };
	int ret;

	if (!addr || !size || !mem_handle || flags)
		return -EINVAL;
	ret = register_mem(bookkeeping_fd, &p);   /* existing helper */
	if (ret)
		return ret;
	map_insert(addr, size, p.mem_handle);
	*mem_handle = p.mem_handle;
	return 0;
}

int nds_unregister_mem(void *addr)
{
	u64 handle;

	if (map_remove(addr, &handle))
		return -ENOENT;
	return unregister_mem(bookkeeping_fd, &(struct p2p_mem_unregister_param){
		.mem_handle = handle,
	});
}

int nds_io_new_ctx(const struct nds_io_ctx_param *param, struct nds_io_ctx **out)
{
	struct nds_io_ctx *ctx;

	if (!param || !out || param->max_io_nr == 0 || param->flags)
		return -EINVAL;
	ctx = alloc_ctx(param->max_io_nr);
	ctx->p2p_fd = new_p2p_fd();
	if (ctx->p2p_fd < 0)
		return ctx->p2p_fd;
	*out = ctx;
	return 0;
}

int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr, struct nds_io_cb *iocb)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		struct io_parameter p;
		int ret;

		if (inflight_full(ctx))
			return i ? (int)i : -EAGAIN;
		ret = validate_and_translate(&iocb[i], &p); /* opcode, obj, topo, mem mode */
		if (ret)
			return i ? (int)i : ret;
		ret = rw_file(ctx->p2p_fd, &p);
		if (ret)
			return i ? (int)i : ret;
		track_inflight(ctx, &iocb[i]);
	}
	return (int)nr;
}

int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr, unsigned int nr,
		     struct nds_io_event *events, struct timespec *timeout)
{
	/* Target: IOCTL_GET_IO_EVENTS — not full-fd drain_io.
	 * See design/p2p_api/completion.md §5.
	 */
	return ioctl_get_io_events(ctx->p2p_fd, min_nr, nr, events, timeout);
}

ssize_t nds_io_pread(struct nds_io_ctx *ctx, struct nds_io_obj *obj,
		     void *buf, size_t len, off_t offset,
		     __u32 flags, __u64 mem_handle)
{
	struct nds_io_cb cb = {
		.opcode = NDS_IO_OP_PREAD,
		.rw_flags = flags,
		.obj = *obj,
		.offset = offset,
		.user_data = 1,
		.buf_addr = (u64)buf,
		.buf_len = len,
		.mem_handle = mem_handle,
	};
	struct nds_io_event ev;
	int n;

	n = nds_io_submit(ctx, 1, &cb);
	if (n != 1)
		return n < 0 ? n : -EIO;
	n = nds_io_getevents(ctx, 1, 1, &ev, NULL);
	if (n != 1)
		return n < 0 ? n : -EIO;
	return ev.res;
}
```

`validate_and_translate` (sketch):

```text
switch opcode:
  PREAD/PWRITE/PREADV/PWRITEV → ok; else -EOPNOTSUPP
unknown rw_flags bits → -EOPNOTSUPP
resolve obj:
  PATH → open; FD → use; ID → -EOPNOTSUPP
if target is bdev → ensure add_topo(cached)
if write && regular file → -EOPNOTSUPP
memory mode:
  if rw_flags & NDS_IO_F_REGISTERED_MEM:
    require mem_handle != 0
    build io_parameter with P2P_IO_F_REGISTERED_MEM + mem_handle
    iov.addr = buf_addr (VA within registration), iov.size = buf_len
  else:
    require mem_handle == 0 and obj.host_pid >= 0
    build io_parameter with flags=0, host_pid from obj
    iov.addr = buf_addr, iov.size = buf_len   # one-shot HBM VA+size
```

Path-based I/O opens inside the library and feeds the existing ioctl packing
path, as locked in [objects.md](p2p_api/objects.md); this does not change the
public `nds_io_cb` layout.

## 4. Ownership summary

| Resource | Owner | Released by |
| --- | --- | --- |
| Process library state | `nds_init` | `nds_shutdown` |
| Addr→handle map entries | Library | `nds_unregister_mem` or shutdown |
| Kernel `mem_handle` / PA pin | Existing registration rules | Unregister / owner p2p fd close |
| `nds_io_ctx` and its p2p fd | Caller | `nds_io_destroy_ctx` |
| Inflight request / HBM buffers | Caller until event returned | Matching `nds_io_event` |
| Topology cache | Library (per ctx or process) | Ctx destroy / shutdown |

## 5. Required implementation areas

The implementation is expected to touch these areas while preserving existing
`file_p2p` behavior and keeping non-completion ioctl packing compatible:

- additive kernel UAPI: submit `user_data`/cookie echo + `IOCTL_GET_IO_EVENTS`
  (see [completion.md](p2p_api/completion.md)); keep `IOCTL_DRAIN_IO` for
  destroy / quiesce;
- new public header (for example `nds_api.h`) and sources under `nds/` or
  `file_p2p/nds_*.c` implementing the facade and v1 backend;
- linkage against existing `file_p2p_api.c` helpers (`new_p2p_fd`, `add_topo`,
  `register_mem`, `rw_file`, …) plus the new getevents wrapper;
- optional thin Python bindings;
- `test/`: **keep** existing basic/stress green; **add** parallel NDS basic +
  NDS stress (and mirrored API/rejection coverage) per
  [testing.md](p2p_api/testing.md);
- keep sources consistent with section 1.1 and `design/p2p_api/*`.

No change to topology discovery meaning, NVMe command construction, or
registered-memory lifetime rules beyond wiring completion cookies through
accepted I/O contexts.

## 6. Acceptance criteria

The feature design (and later its v1-complete implementation) is complete when
all of the following hold:

1. Every GDS API row in section 3.2 has a defined NDS counterpart or an
   explicit NA rationale; `nds_shutdown` and sync pread/pwrite are documented.
2. `nds_init_param`, opcodes, object types, and `nds_io_event` are fully
   specified with reserved-field zero rules and unknown-flag rejection.
3. Synchronous I/O is only submit(1)+getevents(1); apps never use `drain_io`
   or io_uring for NDS completions.
4. Faithful getevents (`min_nr` / `max_nr` / timeout) is backed by
   `IOCTL_GET_IO_EVENTS`, not full-fd drain alone.
5. Both HBM modes work on bdev read/write pipelines via `nds_*`.
6. Unsupported opcodes and `NDS_IO_OBJ_ID` return `-EOPNOTSUPP` before NVMe I/O.
7. `nds_register_mem` returns a usable opaque `mem_handle`; unregister-by-addr
   bridges to the kernel handle; inconsistent flag/handle → `-EINVAL`.
8. One NDS ctx ↔ one p2p fd; buffer lifetime follows getevents.
9. Implementation follows section 1.1 and all `design/p2p_api/` sub-designs.
10. **Existing** `file_p2p` basic + stress still pass; **new** NDS basic +
    NDS stress pass with mirrored coverage ([testing.md](p2p_api/testing.md)).
