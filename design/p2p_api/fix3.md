# fix3 — NDS v2.6 API: init-time topologies, FD-only vector I/O, hidden mem_handle, async-only

Status: authoritative v1 contract implemented in this tree. It supersedes the
conflicting API descriptions in `../p2p_api.md` and the other documents in this
directory, including the per-ctx topology-caching part of `fix1.md` (topology
is registered once at `nds_init`, not per I/O ctx).

The kernel registered-memory UAPI remains handle-based:
`p2p_mem_register_param` returns `mem_handle`, and `p2p_io_param` consumes it.
The NDS userspace API hides that handle in `file_p2p/nds_api.h`,
`file_p2p/nds_api.c`, and `file_p2p/py_nds_api.c`.

## Independence from file_p2p

NDS and `file_p2p` remain **two independent public userspace APIs** on top of
the same kernel UAPI (`p2p_dev_uapi.h`). They share an internal `p2p_common`
layer (`p2p_common.h` / `p2p_common.c`) for `p2p_open_dev` / `p2p_add_topo` /
FIEMAP extent packing (`p2p_prepare_io_extents` / `p2p_get_iov_size`) only.

- NDS still does **not** include `file_p2p_api.h` or link `file_p2p_api.c`.
- Each public API keeps its own surface (`nds_api.h` vs `file_p2p_api.h`) and
  issues the remaining ioctls (`IOCTL_RW_FILE`, mem register/unregister,
  getevents, drain) itself.
- Build wiring: both the `nds` / `file_p2p` Python extensions and the C tests
  link their own API `.c` plus `p2p_common.c`.

## Goals

1. **Topology registration at init time, once**
   - `nds_init_param` carries an `nds_fs_desc` (in the `_data[4]` union):
     `int32_t *fs_fd` + `uint32_t fs_fd_cnt` — an array of file descriptors
     identifying the top device(s) of the I/O topology (regular file on the
     filesystem, or the block device itself). v1 requires `fs_fd_cnt >= 1` and
     registers the topology identified by every supplied descriptor.
   - `nds_init` uses `fstat(fs_fd[i])` to obtain each open file's device identity;
     regular files use `st_dev` and block devices use `st_rdev`. It resolves
     `/sys/dev/block/<maj>:<min>` to `/dev/<base>` and issues `IOCTL_ADD_TOPO`
     on the global `book_fd`, without reopening or restating the file path.
   - All `p2p_fd` I/O contexts created afterwards share the registered topology.
     The per-ctx topo cache from `fix1.md` is removed.
   - Any `add_topo` error is propagated; the current topology path does not
     return `-EEXIST` for an already installed global topology.
   - `nds_init_param.version` is output-only and is set to
     `NDS_API_VERSION` when initialization succeeds.
   - Teardown is `nds_exit()` (renamed from `nds_shutdown`).

2. **FD-only object surface**
   - Object-type constants are removed. `nds_io_obj` contains only an fd plus
     reserved expansion space; paths and future object IDs are not accepted.

3. **`host_pid` moves to `nds_io_cb`**
   - It describes the I/O buffer (who owns the one-shot HBM VAs), not the target
     object, so it lives alongside the I/O vectors in the control block.
   - Userspace still rejects `host_pid < 0` and `host_pid != 0` combined with
     `NDS_IO_F_REGISTERED_MEM`.

4. **`mem_handle` is no longer exposed to userspace**
   - `nds_register_mem(addr, size, flags)` returns only `0`/errno; no out-param.
   - `nds_io_cb` drops `mem_handle`.
   - When a CB carries `NDS_IO_F_REGISTERED_MEM`, the NDS library uses the
     range in `iov[0]` to select a region from its internal
     `addr → mem_handle` table (populated by `nds_register_mem`) and fills
     `p2p_io_param.mem_handle` itself. The kernel then validates every vector
     against the selected region and rejects an out-of-range vector with
     `-ERANGE`.
   - A process-wide mutex protects the internal registration table during
     registration, lookup, removal, and teardown.
   - `nds_unregister_mem` frees the kernel handle on success. If the kernel
     ioctl fails, the library keeps the address-to-handle entry so the caller
     can retry the unregister operation.

5. **Async-only surface: no synchronous helpers**
   - `nds_io_pread` / `nds_io_pwrite` (and their Python `io_pread`/`io_pwrite`
     bindings) are removed. Callers build `nds_io_cb`s, `nds_io_submit()` them,
     and reap completions with `nds_io_getevents()` — the same model as Linux
     AIO. A synchronous convenience layer can be added later on top if needed.
   - `nds_io_event` contains only `user_data`, `res`, and two reserved 64-bit
     words. Its 32-byte size remains identical to the kernel completion event.
   - A completion echoes `user_data`. Its `res` is zero on success or a
     negated Linux errno on failure; it is not a byte count.
   - `max_io_cnt` accepts values from 1 through `NDS_IO_MAX_IO_CNT` (1024) as
     a queue-size hint. The v1 library does not maintain an inflight counter or
     enforce that hint; context destruction always drains the kernel fd.
   - `NDS_IO_OP_PREAD` and `NDS_IO_OP_PWRITE` each accept one or more vectors;
     separate PREADV/PWRITEV opcodes are unnecessary.

## API sketch (new)

```c
#define NDS_IO_MAX_IO_CNT 1024u
#define NDS_IO_MAX_IOV 65536u
#define NDS_IO_OP_PREAD  0u
#define NDS_IO_OP_PWRITE 1u

struct nds_fs_desc {
	int32_t *fs_fd;
	uint32_t fs_fd_cnt;
	uint32_t reserved;
};

struct nds_init_param {
	uint32_t flags;
	union { uint64_t _data[4]; struct nds_fs_desc desc; };
	uint32_t version;          /* output: NDS_API_VERSION */
};

struct nds_io_ctx_param {
	uint32_t max_io_cnt;
	uint32_t flags;
	uint64_t reserved;
};

struct nds_io_obj {
	uint64_t reserved;
	union { uint64_t _data; int32_t fd; };
};

struct nds_io_vec {
	uint64_t buf_addr;
	uint32_t buf_len;
	uint32_t reserved;
};

struct nds_io_cb {
	uint32_t opcode;
	uint32_t rw_flags;        /* NDS_IO_F_REGISTERED_MEM, ... */
	struct nds_io_obj obj;
	uint64_t offset;
	uint64_t user_data;
	const struct nds_io_vec *iov;
	uint32_t iov_cnt;
	int32_t  host_pid;        /* moved here from nds_io_obj */
	uint64_t reserved;
};

struct nds_io_event {
	uint64_t user_data;
	int64_t res;
	uint64_t reserved[2];
};

int nds_init(struct nds_init_param *param);
int nds_exit(void);
int nds_register_mem(void *addr, uint64_t size, int flags);   /* no handle out */
int nds_io_submit(struct nds_io_ctx *ctx, int nr,
		  const struct nds_io_cb *iocb);
int nds_io_getevents(struct nds_io_ctx *ctx, int min_nr, int nr,
		     struct nds_io_event *events, struct timespec *timeout);
/* no nds_io_pread / nds_io_pwrite */
```

`nds_io_vec` is layout-identical to the kernel UAPI `p2p_iov`. The NDS build
asserts their size, alignment, and field offsets, then passes the caller's IOV
array directly to `IOCTL_RW_FILE`; no translation allocation or vector copy is
performed in the library. The ioctl consumes the array before returning. In
v2.6, the former `unsigned long p2p_iov.size` field is split into a `__u32 size`
and must-zero `__u32 reserved`, limiting a vector's representable length to
less than 4 GiB (the kernel currently enforces a 2 GiB maximum). UAPI pointers
for the IOV and completion arrays are encoded as `__u64` userspace addresses so
their containing ioctl structures have the same layout for 32-bit and 64-bit
callers.

## Python binding

- `nds.init(fs_fds[, flags])` — `fs_fds` is a sequence of fds
  (e.g. `[topo_fd]`); empty/invalid entries raise in the binding.
- `nds.exit() -> int`.
- `nds.register_mem(addr, size[, flags]) -> int` (errno-style, no handle).
- Targets are passed directly as nonnegative fds; there is no object-type tuple.
- The optional `host_pid` moved into the iocb tuple:
  `(opcode, fd, offset, iov[, rw_flags=0, host_pid=0, user_data=0])`, where
  `iov` is a sequence of `(buf_addr, buf_len)` pairs.
- `io_pread`/`io_pwrite` are removed from the module.
- All `NDS_IO_OBJ_*` constants are removed from the module.
- `NDS_IO_MAX_IO_CNT` and `NDS_IO_MAX_IOV` are exported by the module;
  PREADV/PWRITEV constants are not defined.

## Error mapping (userspace pre-checks)

| Condition                                   | Return        |
|---------------------------------------------|---------------|
| Negative fd                                 | `-EINVAL`     |
| Empty/oversized IOV                         | `-EINVAL`     |
| I/O offset plus total vector length overflows | `-EOVERFLOW` |
| REGISTERED_MEM + `iov[0]` does not fit a region | `-EINVAL` |
| REGISTERED_MEM + later vector outside selected region | `-ERANGE` |
| REGISTERED_MEM + `host_pid != 0`            | `-EINVAL`     |
| `host_pid < 0`                              | `-EINVAL`     |
| unknown `rw_flags`                          | `-EOPNOTSUPP` |
| init: `fs_fd_cnt == 0` / NULL `fs_fd`       | `-EINVAL`     |
| ctx: `max_io_cnt == 0` or `max_io_cnt > NDS_IO_MAX_IO_CNT` | `-EINVAL` |
| negative submit/getevents counts            | `-EINVAL`     |
| any nonzero reserved input field            | `-EINVAL`     |

Everything else (alignment, extent coverage, op validity, device capability)
remains the kernel's responsibility; NDS just propagates the errno.

## Tests

- `nds_api_test.c` / `python_nds_api_test.py` open every manifest path
  (`O_RDONLY|O_DIRECT`) and pass the fd; `--topology` is opened once and handed
  to `nds_init` as the single element of `desc.fs_fd`.
- Reject mode updated to the new surface:
  `reject-reg-unregistered-buf` (replaces `reject-reg-zero-handle` and
  `reject-oneshot-with-handle`), `reject-host-pid-neg`,
  `reject-reg-host-pid`, `reject-unknown-rw-flags`, reserved-field checks,
  Python integer-overflow checks, init/ctx range checks, negative completion
  results, and `reject-getevents-empty`.
- Completion tests match each event to its request by `user_data` and verify
  `res` and reserved output fields, including queued
  completions that may arrive out of order.
- C and Python completion harvest loops use a 300-second timeout so a lost
  completion fails the suite instead of hanging indefinitely, while leaving
  enough headroom for KASAN and stress configurations.
- The reject names are not consumed by `report_test.py`; the report tracks only
  per-case `ret`/`expected` pairs, so no report changes were needed.

## Related designs

- Kernel completion queue and getevents (AIO / io_uring aligned target):
  [completion_getevents.md](completion_getevents.md).
