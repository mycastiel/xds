# NDS UAPI and userspace contract

> **Status:** Historical proposal. The authoritative implemented v1 contract
> is [fix3](fix3.md); conflicting interface descriptions below are superseded.

This document is part of the [p2p_api](../p2p_api.md) design.  It must not be
implemented in isolation from the [backend](backend.md),
[completion](completion.md), [objects](objects.md), or [testing](testing.md)
contracts.

## 1. Goals

The public NDS header (`nds_api.h`) is the GDS-aligned surface for XDS.  It
exposes init/shutdown, HBM registration, I/O contexts, submit/getevents, and
thin synchronous helpers.

The chosen interface has these properties:

- negative Linux errno on failure, matching `file_p2p`;
- both registered-handle and one-shot HBM VA+size modes as explicit,
  first-class submit options;
- no `cuFileHandleRegister` equivalent (each I/O carries `nds_io_obj`);
- additive kernel completion support for faithful getevents (see §2); existing
  topology / RW / register ioctl layouts stay compatible; and
- reserved fields and unknown flags rejected before any hardware I/O.

## 2. Kernel UAPI relationship

Applications never call kernel ioctls directly when using NDS.  The library
maps onto:

| Kernel ioctl | Used by NDS for |
| --- | --- |
| `IOCTL_ADD_TOPO` | Automatic topology registration (library) |
| `IOCTL_RW_FILE` | Async read/write after translating `nds_io_cb` |
| `IOCTL_GET_IO_EVENTS` | **New** — faithful `nds_io_getevents` (`min_nr` / `max_nr` / timeout) |
| `IOCTL_DRAIN_IO` | Destroy / full quiesce only — **not** the getevents engine |
| `IOCTL_REGISTER_MEM` | `nds_register_mem` |
| `IOCTL_UNREGISTER_MEM` | `nds_unregister_mem` |

`IOCTL_DRAIN_IO` alone cannot implement getevents: it waits for **all**
published I/O on the fd, with no `min_nr` and no timeout.  Details and the
required submit-time `user_data` / cookie echo are in
[completion.md](completion.md) §5.

**Additive UAPI for v1-complete** (exact field layout in the completion /
kernel UAPI patch):

- echo `__u64 user_data` (or opaque `io_id`) on `IOCTL_RW_FILE` accept so
  completions can be correlated;
- `IOCTL_GET_IO_EVENTS` returning up to `max_nr` `p2p_io_event` records after
  waiting for at least `min_nr` or timeout.

Do **not** reshape ADD_TOPO / REGISTER_MEM / the core RW extent packing beyond
these additive completion fields.

## 3. Public constants and structures

```c
#define NDS_API_VERSION 1u

struct nds_init_param {
	__u32 version;     /* must be NDS_API_VERSION */
	__u32 flags;       /* v1: must be 0 */
	__u64 reserved[4]; /* must be zero */
};

struct nds_io_ctx_param {
	__u32 max_io_nr;
	__u32 flags;       /* v1: must be 0 */
	__u64 pad;         /* must be zero */
};

/* Opaque library object; callers hold struct nds_io_ctx *. */
struct nds_io_ctx;

#define NDS_IO_OBJ_FD   1u
#define NDS_IO_OBJ_PATH 2u
#define NDS_IO_OBJ_ID   3u  /* v1: -EOPNOTSUPP */

struct nds_io_obj {
	__u32 type;
	__s32 host_pid;    /* 0 = current; must be 0 with REGISTERED_MEM */
	union {
		__u64 id;
		const char *str;
		__s32 fd;
	};
	__u64 pad2;        /* must be zero */
};

#define NDS_IO_OP_PREAD    0u
#define NDS_IO_OP_PWRITE   1u
#define NDS_IO_OP_PREADV   2u
#define NDS_IO_OP_PWRITEV  3u

#define NDS_IO_F_REGISTERED_MEM (1u << 0)
#define NDS_IO_F_MASK           NDS_IO_F_REGISTERED_MEM

struct nds_io_cb {
	__u32 opcode;
	__u32 rw_flags;
	struct nds_io_obj obj;
	__u64 offset;
	__u64 user_data;
	__u64 buf_addr;
	__u32 buf_len;
	__u32 pad1;        /* must be zero */
	__u64 mem_handle;
	__u64 pad2;        /* must be zero */
};

struct nds_io_event {
	__u64 user_data;
	__s64 res;         /* 0 on success; <0 negated errno */
	__u32 opcode;
	__u32 flags;       /* v1: 0 on output */
	__u64 reserved[2]; /* must be zero on input if ever reused as in-param */
};
```

### 3.1 Buffer fields

| Opcode | `buf_addr` | `buf_len` |
| --- | --- | --- |
| `PREAD` / `PWRITE` | HBM VA of one contiguous range | byte length |
| `PREADV` / `PWRITEV` | pointer to `{addr,len}` array | number of entries |

Sector alignment and nonzero length rules follow the existing `rw_file` /
`p2p_iov` contract after translation.

## 4. Memory modes

| Mode | `rw_flags` | `mem_handle` | `buf_addr` / `buf_len` | Kernel path |
| --- | --- | --- | --- | --- |
| Registered | `NDS_IO_F_REGISTERED_MEM` | nonzero from `nds_register_mem` | VA within registered range + length | `P2P_IO_F_REGISTERED_MEM` + cached PA |
| One-shot | `0` | must be `0` | HBM VA + size for this I/O | per-I/O `devmm_get_mem_pa_list` |

Validation before submit:

- unknown `rw_flags` bits → `-EOPNOTSUPP`;
- registered with `mem_handle == 0` → `-EINVAL`;
- one-shot with `mem_handle != 0` → `-EINVAL`;
- the library must **not** choose the mode by probing whether `buf_addr` falls
  in a prior registration.

`nds_register_mem` writes the opaque handle to `*mem_handle` on success.
`nds_unregister_mem(void *addr)` deregisters by the registration **base
address** (GDS-shaped); the library maps `addr → mem_handle` internally.

## 5. Public functions

```c
int nds_init(struct nds_init_param *param);
int nds_shutdown(void);

int nds_register_mem(void *addr, __u64 size, int flags, __u64 *mem_handle);
int nds_unregister_mem(void *addr);

int nds_io_new_ctx(const struct nds_io_ctx_param *param,
		   struct nds_io_ctx **ctx);
int nds_io_destroy_ctx(struct nds_io_ctx *ctx);

int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr,
		  struct nds_io_cb *iocb);
int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout);

ssize_t nds_io_pread(struct nds_io_ctx *ctx, struct nds_io_obj *obj,
		     void *buf, size_t len, off_t offset,
		     __u32 flags, __u64 mem_handle);
ssize_t nds_io_pwrite(struct nds_io_ctx *ctx, struct nds_io_obj *obj,
		      const void *buf, size_t len, off_t offset,
		      __u32 flags, __u64 mem_handle);
```

### 5.1 Return conventions

| Call | Success | Failure |
| --- | --- | --- |
| Most `int` APIs | `0` | negative errno |
| `nds_io_submit` | number of accepted requests from the head of `iocb` | negative errno if none accepted |
| `nds_io_getevents` | number of events written (may be `0` on timeout) | negative errno |
| `nds_io_pread` / `pwrite` | byte count (`res`) | negative errno |

`nds_init`: `version` must be `NDS_API_VERSION`; nonzero `flags` or reserved →
`-EOPNOTSUPP` / `-EINVAL` as appropriate.  Double init without shutdown is
`-EBUSY`.

`nds_io_new_ctx`: `max_io_nr == 0` or nonzero `flags`/`pad` → `-EINVAL`.

## 6. GDS alignment

| GDS | NDS | Notes |
| --- | --- | --- |
| `cuFileDriverOpen` | `nds_init` | |
| `cuFileDriverClose` | `nds_shutdown` | |
| `cuFileHandleRegister` | **NA** | use `nds_io_obj` per I/O |
| `cuFileHandleDeregister` | **NA** | |
| `cuFileBufRegister` | `nds_register_mem` | returns handle out-param |
| `cuFileBufDeregister` | `nds_unregister_mem` | by base address |
| `cuFileBatchIOSetup` | `nds_io_new_ctx` | |
| `cuFileBatchIODestroy` | `nds_io_destroy_ctx` | |
| `cuFileBatchIOSubmit` | `nds_io_submit` | |
| `cuFileBatchIOGetStatus` | `nds_io_getevents` | |
| `cuFileRead` / `Write` | `nds_io_pread` / `pwrite` | submit+getevents only |

## 7. Error behavior summary

| Condition | errno |
| --- | --- |
| Bad version / null pointers / zero sizes | `-EINVAL` |
| Unknown flags or unsupported opcode/obj type | `-EOPNOTSUPP` |
| Inconsistent registered/one-shot fields | `-EINVAL` |
| Inflight capacity exhausted at submit head | `-EAGAIN` |
| Write to regular file | `-EOPNOTSUPP` |
| Missing topology / unsupported layout | as returned by `add_topo` / kernel |
| Stale or foreign `mem_handle` | as returned by kernel (`-ENOENT` / `-EPERM`, etc.) |

Library paths should `fprintf(stderr, ...)` on failure in the same spirit as
`file_p2p_api.c`, without changing the returned errno.

## 8. Non-goals

- Reshaping ADD_TOPO / REGISTER_MEM / extent packing beyond the additive
  completion cookie and `IOCTL_GET_IO_EVENTS` described above.
- Exporting `drain_io` or io_uring to applications as a getevents substitute.
- Auto-inferring memory mode from buffer addresses.
- KV/object backends (`NDS_IO_OBJ_ID`) beyond returning `-EOPNOTSUPP`.
- Claiming timed/`min_nr` getevents while only calling full-fd `IOCTL_DRAIN_IO`.
