# fix3 — NDS API reshape: init-time topology (fs_fd array), FD-only objects, host_pid on CB, hidden mem_handle, async-only

Status: implemented in this tree. Supersedes the per-ctx topology-caching part of
`fix1.md` (topology is now registered once at `nds_init`, not per I/O ctx).

Kernel UAPI is unchanged: `p2p_mem_register_param` still returns `mem_handle`,
and `p2p_io_param` still consumes it. The changes are confined to the NDS
userspace API (`file_p2p/nds_api.h`, `file_p2p/nds_api.c`, `file_p2p/py_nds_api.c`)
and its tests.

## Independence from file_p2p

NDS and `file_p2p` are **two independent userspace APIs** that both sit on top
of the same kernel UAPI (`p2p_dev_uapi.h`). They evolve separately:

- `nds_api.c` is self-contained. It opens `/dev/p2p_device`, does its own
  topology discovery (nvme/linear/raid0 sysfs+DM+MD walking), its own
  FIEMAP extent packing, and issues `IOCTL_ADD_TOPO` / `IOCTL_RW_FILE` /
  `IOCTL_REGISTER_MEM` / `IOCTL_UNREGISTER_MEM` / `IOCTL_GET_IO_EVENTS` /
  `IOCTL_DRAIN_IO` directly. It does **not** include `file_p2p_api.h` or link
  `file_p2p_api.c`.
- `file_p2p_api.c` is untouched and keeps serving the C API / UAPI tests.
- Build wiring reflects the split: the `nds` Python extension and
  `nds_api_test` link only `nds_api.c` (+ `py_nds_api.c`), not
  `file_p2p_api.c`.

## Goals

1. **Topology registration at init time, once**
   - `nds_init_param` carries an `nds_fs_desc` (in the `_data[4]` union):
     `int32_t *fs_fd` + `uint32_t fs_fd_cnt` — an array of file descriptors
     identifying the top device(s) of the I/O topology (regular file on the
     filesystem, or the block device itself). v1 requires `fs_fd_cnt == 1` and
     registers that single topology; the array form leaves room for
     multi-device topologies.
   - `nds_init` resolves `fs_fd[0]` to the underlying block device path
     (`/proc/self/fd` → stat → regular files resolve via `/sys/dev/block/<maj>:<min>`
     to `/dev/<base>`) and issues `IOCTL_ADD_TOPO` on the global `book_fd`.
   - All `p2p_fd` I/O contexts created afterwards share the registered topology.
     The per-ctx topo cache from `fix1.md` is removed.
   - `add_topo` returning `-EEXIST` is treated as success (topology already
     registered by a previous init / raw UAPI user).
   - Teardown is `nds_exit()` (renamed from `nds_shutdown`).

2. **FD-only object surface**
   - `NDS_IO_OBJ_PATH` is removed; `NDS_IO_OBJ_FD = 1`, `NDS_IO_OBJ_ID = 2`.
   - The `nds_io_obj` union is `{ uint64_t _data; int32_t fd; }` — no named
     `str`/`id` members in this version; `NDS_IO_OBJ_ID` resolves to
     `-EOPNOTSUPP` (unchanged).

3. **`host_pid` moves to `nds_io_cb`**
   - It describes the I/O buffer (who owns the one-shot HBM VAs), not the target
     object, so it lives alongside `buf_addr` in the control block.
   - Userspace still rejects `host_pid < 0` and `host_pid != 0` combined with
     `NDS_IO_F_REGISTERED_MEM`.

4. **`mem_handle` is no longer exposed to userspace**
   - `nds_register_mem(addr, size, flags)` returns only `0`/errno; no out-param.
   - `nds_io_cb` drops `mem_handle`.
   - When a CB carries `NDS_IO_F_REGISTERED_MEM`, the NDS library looks up the
     registered region containing `buf_addr` in its internal
     `addr → mem_handle` table (populated by `nds_register_mem`) and fills
     `p2p_io_param.mem_handle` itself. A `buf_addr` outside every registered
     region is rejected with `-EINVAL`.
   - `nds_unregister_mem` frees the kernel handle exactly as before.

5. **Async-only surface: no synchronous helpers**
   - `nds_io_pread` / `nds_io_pwrite` (and their Python `io_pread`/`io_pwrite`
     bindings) are removed. Callers build `nds_io_cb`s, `nds_io_submit()` them,
     and reap completions with `nds_io_getevents()` — the same model as Linux
     AIO. A synchronous convenience layer can be added later on top if needed.
   - `nds_io_event` drops its unused `flags` field; padding fields were
     renamed to `reserved`/`reserved1`/`reserved2`/`_data` throughout.

## API sketch (new)

```c
struct nds_fs_desc {
	int32_t *fs_fd;
	uint32_t fs_fd_cnt;
	uint32_t reserved;
};

struct nds_init_param {
	uint32_t flags;
	union { uint64_t _data[4]; struct nds_fs_desc desc; };
	uint32_t version;
};

struct nds_io_obj {           /* FD or (future) ID/KV */
	uint32_t type;            /* NDS_IO_OBJ_FD | NDS_IO_OBJ_ID */
	uint32_t reserved1;
	union { uint64_t _data; int32_t fd; };
	uint64_t reserved2;
};

struct nds_io_cb {
	uint32_t opcode;
	uint32_t rw_flags;        /* NDS_IO_F_REGISTERED_MEM, ... */
	struct nds_io_obj obj;
	uint64_t offset;
	uint64_t user_data;
	uint64_t buf_addr;
	uint32_t buf_len;
	int32_t  host_pid;        /* moved here from nds_io_obj */
	uint64_t reserved;
};

int nds_init(struct nds_init_param *param);
int nds_exit(void);
int nds_register_mem(void *addr, uint64_t size, int flags);   /* no handle out */
int nds_io_submit(struct nds_io_ctx *ctx, unsigned nr, struct nds_io_cb *iocb);
int nds_io_getevents(struct nds_io_ctx *ctx, unsigned min_nr, unsigned nr,
		     struct nds_io_event *events, struct timespec *timeout);
/* no nds_io_pread / nds_io_pwrite */
```

## Python binding

- `nds.init(fs_fds[, version[, flags]])` — `fs_fds` is a sequence of fds
  (e.g. `[topo_fd]`); empty/invalid entries raise in the binding.
- `nds.exit() -> int`.
- `nds.register_mem(addr, size[, flags]) -> int` (errno-style, no handle).
- Object tuple: `(NDS_IO_OBJ_FD, fd)` / `(NDS_IO_OBJ_ID, id)` — ID is passed
  through to C, which returns `-EOPNOTSUPP`.
- The optional `host_pid` moved into the iocb tuple:
  `(opcode, obj, offset, buf_addr, buf_len[, rw_flags=0, host_pid=0, user_data=0])`.
- `io_pread`/`io_pwrite` are removed from the module.
- The `NDS_IO_OBJ_PATH` constant is removed from the module.

## Error mapping (userspace pre-checks)

| Condition                                   | Return        |
|---------------------------------------------|---------------|
| `NDS_IO_OBJ_ID` object                      | `-EOPNOTSUPP` |
| REGISTERED_MEM + unregistered `buf_addr`    | `-EINVAL`     |
| REGISTERED_MEM + `host_pid != 0`            | `-EINVAL`     |
| `host_pid < 0`                              | `-EINVAL`     |
| unknown `rw_flags`                          | `-EOPNOTSUPP` |
| bad/unsupported object type                 | `-EINVAL`     |
| init: `fs_fd_cnt != 1` / NULL `fs_fd`       | `-EINVAL`     |

Everything else (alignment, extent coverage, op validity, device capability)
remains the kernel's responsibility; NDS just propagates the errno.

## Tests

- `nds_api_test.c` / `python_nds_api_test.py` open every manifest path
  (`O_RDONLY|O_DIRECT`) and pass the fd; `--topology` is opened once and handed
  to `nds_init` as the single element of `desc.fs_fd`.
- Reject mode updated to the new surface:
  `reject-reg-unregistered-buf` (replaces `reject-reg-zero-handle` and
  `reject-oneshot-with-handle`), `reject-obj-id`, `reject-host-pid-neg`,
  `reject-reg-host-pid`, `reject-unknown-rw-flags`, `reject-getevents-empty`.
- The reject names are not consumed by `report_test.py`; the report tracks only
  per-case `ret`/`expected` pairs, so no report changes were needed.

