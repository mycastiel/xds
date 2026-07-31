#ifndef __NDS_API_H__
#define __NDS_API_H__

#include <stdint.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <time.h>	/* struct timespec for nds_io_getevents */

/*
 * NDS — userspace API for NPU HBM ↔ NVMe P2P I/O (GDS-like, async).
 *
 * Typical usage:
 *
 *   1. Open a file or block device that sits on the target topology, then:
 *        int fds[] = { topo_fd };
 *        struct nds_init_param ip = {
 *            .version = NDS_API_VERSION,
 *            .desc = { .fs_fd = fds, .fs_fd_cnt = 1 },
 *        };
 *        nds_init(&ip);
 *
 *   2. (Optional) Register HBM / CMB windows once:
 *        nds_register_mem(addr, size, 0);
 *      Later I/Os that use those VAs set NDS_IO_F_REGISTERED_MEM; the library
 *      looks up the handle — callers never see mem_handle.
 *
 *   3. Create an I/O context and submit work (Linux-AIO style):
 *        struct nds_io_ctx *ctx;
 *        nds_io_new_ctx(&(struct nds_io_ctx_param){ .max_io_nr = N }, &ctx);
 *
 *        struct nds_io_cb cb = {
 *            .opcode   = NDS_IO_OP_PREAD,
 *            .obj      = { .type = NDS_IO_OBJ_FD, .fd = file_fd },
 *            .offset   = ...,
 *            .buf_addr = ...,
 *            .buf_len  = ...,
 *            .rw_flags = 0,          // or NDS_IO_F_REGISTERED_MEM
 *            .host_pid = 0,          // must be 0 with REGISTERED_MEM
 *            .user_data = cookie,    // echoed in completion events
 *        };
 *        nds_io_submit(ctx, 1, &cb);
 *        nds_io_getevents(ctx, 1, 1, events, NULL);
 *
 *   4. Tear down: destroy ctx → unregister mem → nds_exit().
 *
 * Conventions:
 *   - All functions return 0 / positive count on success, or -errno on failure.
 *   - I/O is async only (no synchronous pread/pwrite helpers).
 *   - Targets are FDs (NDS_IO_OBJ_FD). Paths are not accepted.
 *   - Offsets / lengths / buffer addresses must be sector-aligned (512 B).
 *   - reserved / _data fields must be zero.
 */

#define NDS_API_VERSION 1u

struct nds_io_ctx {
	int p2p_fd;
	unsigned int max_io_nr;
	_Atomic unsigned int inflight;
};

/*
 * Topology descriptor for nds_init.
 * v1: fs_fd_cnt must be 1; fs_fd[0] is a regular-file or block-device fd
 * whose underlying block device is registered via add_topo once for the
 * process. The array form is reserved for future multi-device topologies.
 */
struct nds_fs_desc {
	int32_t *fs_fd;
	uint32_t fs_fd_cnt;
	uint32_t reserved;
};

struct nds_init_param {
	uint32_t flags;			/* must be 0 in v1 */
	union {
		uint64_t _data[4];	/* must be zero when unused */
		struct nds_fs_desc desc;
	};
	uint32_t version;		/* must be NDS_API_VERSION */
};

/* max_io_nr: upper bound on outstanding I/Os in this context. flags = 0.
 * A ctx may be shared across threads: inflight is an atomic counter so
 * concurrent submit/getevents cannot race the outstanding-I/O budget.
 * The kernel serializes the actual I/O on the ctx's p2p_fd. */
struct nds_io_ctx_param {
	uint32_t max_io_nr;
	uint32_t flags;
	uint64_t reserved;
};

/* I/O target. Only NDS_IO_OBJ_FD is supported; NDS_IO_OBJ_ID → -EOPNOTSUPP. */
#define NDS_IO_OBJ_FD   1u
#define NDS_IO_OBJ_ID   2u

struct nds_io_obj {
	uint32_t type;			/* NDS_IO_OBJ_FD or NDS_IO_OBJ_ID */
	uint32_t reserved1;
	union {
		uint64_t _data;
		int32_t fd;		/* open file / block-device fd */
	};
	uint64_t reserved2;
};

#define NDS_IO_OP_PREAD    0u		/* single-buffer read  */
#define NDS_IO_OP_PWRITE   1u		/* single-buffer write (block dev only) */
#define NDS_IO_OP_PREADV   2u		/* reserved; not supported yet */
#define NDS_IO_OP_PWRITEV  3u		/* reserved; not supported yet */

/* Buffer was previously registered with nds_register_mem(). */
#define NDS_IO_F_REGISTERED_MEM (1u << 0)
#define NDS_IO_F_MASK           NDS_IO_F_REGISTERED_MEM

/*
 * One async I/O request. Fill an array of these and pass to nds_io_submit().
 *
 * buf_addr / buf_len describe the HBM (or CMB) VA window for this transfer.
 * With NDS_IO_F_REGISTERED_MEM, buf_addr must fall inside a region previously
 * passed to nds_register_mem(); otherwise the buffer is treated as one-shot.
 */
struct nds_io_cb {
	uint32_t opcode;		/* NDS_IO_OP_* */
	uint32_t rw_flags;		/* NDS_IO_F_* bitmask */
	struct nds_io_obj obj;
	uint64_t offset;		/* byte offset in the target object */
	uint64_t user_data;		/* opaque cookie, returned in events */
	uint64_t buf_addr;		/* HBM/CMB virtual address */
	uint32_t buf_len;		/* transfer length in bytes */
	/*
	 * Owner of one-shot HBM VAs. 0 = current process.
	 * Must be 0 when NDS_IO_F_REGISTERED_MEM is set.
	 */
	int32_t host_pid;
	uint64_t reserved;
};

/* One completion. Layout matches kernel struct p2p_io_event (32B).
 * res is success(==0) or -errno. */
struct nds_io_event {
	uint64_t user_data;
	int64_t res;
	uint32_t opcode;
	uint32_t flags;			/* v1: 0 */
	uint64_t reserved2[2];
};

/* Process-wide init / teardown. nds_init is not re-entrant; call once. */
int nds_init(struct nds_init_param *param);
int nds_exit(void);

/*
 * Pin / unpin an HBM (or CMB) VA range for P2P I/O.
 * flags must be 0. No handle is returned — use NDS_IO_F_REGISTERED_MEM and
 * the same buf_addr on later submits. Unregister by the same addr pointer.
 */
int nds_register_mem(void *addr, uint64_t size, int flags);
int nds_unregister_mem(void *addr);

/* Create / destroy a per-queue I/O context. Destroy drains outstanding I/O. */
int nds_io_new_ctx(const struct nds_io_ctx_param *param,
		   struct nds_io_ctx **ctx);
int nds_io_destroy_ctx(struct nds_io_ctx *ctx);

/*
 * Submit nr control blocks. Returns number submitted, or -errno.
 * Completions are reaped with nds_io_getevents().
 */
int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr,
		  struct nds_io_cb *iocb);

/*
 * Wait for at least min_nr completions, up to nr.
 * timeout NULL = wait forever; {0,0} = non-blocking.
 * On success returns the number of events filled (>= 0); on failure -errno.
 */
int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout);

#endif
