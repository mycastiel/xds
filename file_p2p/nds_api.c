/*
 * NDS — a GDS-aligned userspace API for NPU HBM ↔ NVMe P2P I/O.
 *
 * NDS does not link against file_p2p_api; it shares p2p_common with file_p2p
 * for device open / topo / FIEMAP only. The two userspace APIs evolve
 * independently on top of the same kernel UAPI (p2p_dev_uapi.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>

#include <linux/fiemap.h>

#include "nds_api.h"
#include "p2p_dev_uapi.h"
#include "p2p_common.h"

struct nds_reg_entry {
	struct nds_reg_entry *next;
	void *addr;
	uint64_t size;
	uint64_t handle;
};

struct nds_state {
	int book_fd;
	struct nds_reg_entry *regs;
};

struct nds_io_ctx {
	int p2p_fd;
	unsigned int max_io_nr;
	unsigned int inflight;
};

static struct nds_state g_state = { .book_fd = -1 };

static int is_init(void)
{
	return g_state.book_fd >= 0;
}

/* ------------------------------------------------------------------ */
/* Public NDS API                                                     */
/* ------------------------------------------------------------------ */

int nds_init(struct nds_init_param *param)
{
	char path[PATH_MAX];
	char bdev[PATH_MAX];
	struct stat st;
	dev_t target;
	char sys[64];
	char link[PATH_MAX];
	const char *base;
	const char *topo_path;
	ssize_t n;
	int32_t topo_fd;
	int ret;

	if (!param)
		return -EINVAL;
	if (param->version != NDS_API_VERSION)
		return -EINVAL;
	if (param->flags)
		return -EOPNOTSUPP;
	if (is_init())
		return -EBUSY;

	/*
	 * Topology is described by an fs_fd array; v1 registers the topology
	 * of the first fd only (single top device).
	 */
	if (!param->desc.fs_fd || param->desc.fs_fd_cnt != 1 ||
	    param->desc.reserved)
		return -EINVAL;
	topo_fd = param->desc.fs_fd[0];

	/* Resolve topo fd to the underlying block device path. */
	snprintf(path, sizeof(path), "/proc/self/fd/%d", topo_fd);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return -errno;
	link[n] = '\0';
	if (stat(link, &st) < 0)
		return -errno;
	if (S_ISBLK(st.st_mode)) {
		topo_path = link;
	} else if (S_ISREG(st.st_mode)) {
		target = st.st_dev;
		snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u",
			 major(target), minor(target));
		n = readlink(sys, link, sizeof(link) - 1);
		if (n < 0)
			return -errno;
		link[n] = '\0';
		base = strrchr(link, '/');
		base = base ? base + 1 : link;
		if (snprintf(bdev, sizeof(bdev), "/dev/%s", base) >=
		    (int)sizeof(bdev))
			return -ENAMETOOLONG;
		topo_path = bdev;
	} else {
		return -EINVAL;
	}

	g_state.book_fd = p2p_open_dev();
	if (g_state.book_fd < 0)
		return g_state.book_fd;
	g_state.regs = NULL;

	/* Register topology once for the lifetime of book_fd. */
	ret = p2p_add_topo(g_state.book_fd, topo_path);
	if (ret && ret != -EEXIST) {
		close(g_state.book_fd);
		g_state.book_fd = -1;
		return ret;
	}
	return 0;
}

int nds_exit(void)
{
	struct nds_reg_entry *r, *next;

	if (!is_init())
		return -EINVAL;

	for (r = g_state.regs; r; r = next) {
		struct p2p_mem_unregister_param up = {
			.mem_handle = r->handle,
			.reserved = 0,
		};
		next = r->next;
		ioctl(g_state.book_fd, IOCTL_UNREGISTER_MEM, &up);
		free(r);
	}
	g_state.regs = NULL;

	close(g_state.book_fd);
	g_state.book_fd = -1;
	return 0;
}

int nds_register_mem(void *addr, uint64_t size, int flags)
{
	struct p2p_mem_register_param p = { 0 };
	struct nds_reg_entry *r;

	if (!is_init())
		return -EINVAL;
	/* addr may be 0 (stub CMB window starts at VA 0). */
	if (!size || flags)
		return -EINVAL;

	p.addr = (__u64)(uintptr_t)addr;
	p.size = size;
	if (ioctl(g_state.book_fd, IOCTL_REGISTER_MEM, &p) < 0)
		return -errno;

	r = calloc(1, sizeof(*r));
	if (!r) {
		struct p2p_mem_unregister_param up = { .mem_handle = p.mem_handle };
		ioctl(g_state.book_fd, IOCTL_UNREGISTER_MEM, &up);
		return -ENOMEM;
	}
	r->addr = addr;
	r->size = size;
	r->handle = p.mem_handle;
	r->next = g_state.regs;
	g_state.regs = r;

	return 0;
}

int nds_unregister_mem(void *addr)
{
	struct nds_reg_entry *r, **prev;
	struct p2p_mem_unregister_param up;
	int ret = 0;

	if (!is_init())
		return -EINVAL;

	for (prev = &g_state.regs; *prev; prev = &(*prev)->next) {
		if ((*prev)->addr == addr)
			break;
	}
	if (!*prev)
		return -ENOENT;

	r = *prev;
	*prev = r->next;

	up.mem_handle = r->handle;
	up.reserved = 0;
	if (ioctl(g_state.book_fd, IOCTL_UNREGISTER_MEM, &up) < 0)
		ret = -errno;
	free(r);
	return ret;
}

int nds_io_new_ctx(const struct nds_io_ctx_param *param, struct nds_io_ctx **ctx)
{
	struct nds_io_ctx *c;
	int fd;

	if (!is_init() || !param || !ctx)
		return -EINVAL;
	if (param->max_io_nr == 0 || param->flags || param->reserved)
		return -EINVAL;

	fd = p2p_open_dev();
	if (fd < 0)
		return fd;

	c = calloc(1, sizeof(*c));
	if (!c) {
		close(fd);
		return -ENOMEM;
	}
	c->p2p_fd = fd;
	c->max_io_nr = param->max_io_nr;
	*ctx = c;
	return 0;
}

int nds_io_destroy_ctx(struct nds_io_ctx *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;

	if (ctx->inflight) {
		ret = ioctl(ctx->p2p_fd, IOCTL_DRAIN_IO);
		if (ret < 0)
			fprintf(stderr, "nds: destroy_ctx drain warning %d\n",
				-errno);
	}
	ret = close(ctx->p2p_fd);
	free(ctx);
	return ret;
}

static int resolve_obj_to_fd(struct nds_io_obj *obj, int *fd_out)
{
	if (!obj)
		return -EINVAL;
	if (obj->type == NDS_IO_OBJ_FD) {
		if (obj->fd < 0)
			return -EINVAL;
		*fd_out = obj->fd;
		return 0;
	}
	if (obj->type == NDS_IO_OBJ_ID)
		return -EOPNOTSUPP;
	return -EINVAL;
}

static int nds_iocb_to_io_param(struct nds_io_cb *cb, int file_fd,
				struct p2p_io_param **io_out,
				struct p2p_iov *iov)
{
	struct nds_reg_entry *r;
	struct p2p_io_param *io;
	struct fiemap *exts = NULL;
	struct stat file_stat;
	unsigned long io_size;
	unsigned long long total_size = 0;
	size_t request_size;
	unsigned int ext_num = 0;
	uint64_t buf_addr;
	int err;

	if (cb->opcode > NDS_IO_OP_PWRITEV)
		return -EOPNOTSUPP;
	if (cb->opcode == NDS_IO_OP_PREADV || cb->opcode == NDS_IO_OP_PWRITEV)
		return -EOPNOTSUPP; /* TODO */
	if (cb->rw_flags & ~NDS_IO_F_MASK)
		return -EOPNOTSUPP;

	iov->addr = cb->buf_addr;
	iov->size = cb->buf_len;
	err = p2p_get_iov_size(iov, 1, &io_size);
	if (err)
		return err;
	if (ULONG_MAX - cb->offset < io_size)
		return -EOVERFLOW;

	if (fstat(file_fd, &file_stat) < 0) {
		err = -errno;
		fprintf(stderr, "nds: fstat fd %d failed, errno: %d\n",
			file_fd, err);
		return err;
	}

	if ((cb->opcode == NDS_IO_OP_PWRITE) && !S_ISBLK(file_stat.st_mode)) {
		return S_ISREG(file_stat.st_mode) ? -EOPNOTSUPP : -EINVAL;
	}

	err = p2p_prepare_io_extents(file_fd, &file_stat, cb->offset, io_size,
				     &exts, &ext_num, &total_size);
	if (err) {
		fprintf(stderr, "nds: prepare extents failed, errno: %d\n", err);
		return err;
	}
	if (!ext_num || total_size < io_size) {
		fprintf(stderr, "nds: extent size %llu < IOV size %lu\n",
			total_size, io_size);
		free(exts);
		return -ENODATA;
	}

	if (__builtin_mul_overflow((size_t)ext_num, sizeof(io->extents[0]),
				   &request_size) ||
	    __builtin_add_overflow(request_size, sizeof(*io), &request_size)) {
		free(exts);
		return -E2BIG;
	}
	io = calloc(1, request_size);
	if (!io) {
		free(exts);
		return -ENOMEM;
	}

	io->op = (cb->opcode == NDS_IO_OP_PWRITE) ? P2P_IO_WRITE : P2P_IO_READ;
	io->file_fd = file_fd;
	io->user_data = cb->user_data;
	io->iov = iov;
	io->iov_nr = 1;
	io->ext_nr = ext_num;
	memcpy(io->extents, exts->fm_extents, ext_num * sizeof(io->extents[0]));
	free(exts);

	if (cb->rw_flags & NDS_IO_F_REGISTERED_MEM) {
		if (cb->host_pid) {
			free(io);
			return -EINVAL;
		}
		buf_addr = cb->buf_addr;
		for (r = g_state.regs; r; r = r->next) {
			if (buf_addr >= (uint64_t)(uintptr_t)r->addr &&
			    buf_addr < (uint64_t)(uintptr_t)r->addr + r->size)
				break;
		}
		if (!r) {
			free(io);
			return -EINVAL;
		}
		io->flags = P2P_IO_F_REGISTERED_MEM;
		io->host_pid = 0;
		io->mem_handle = r->handle;
	} else {
		if (cb->host_pid < 0) {
			free(io);
			return -EINVAL;
		}
		io->flags = 0;
		io->host_pid = cb->host_pid ? cb->host_pid : getpid();
		io->mem_handle = 0;
	}

	*io_out = io;
	return 0;
}

int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr, struct nds_io_cb *iocb)
{
	unsigned int i;
	int ret;

	if (!ctx || !iocb)
		return -EINVAL;
	if (nr == 0)
		return 0;

	for (i = 0; i < nr; i++) {
		struct p2p_io_param *io = NULL;
		struct p2p_iov iov;
		struct nds_io_cb *cb = &iocb[i];
		int file_fd;

		if (ctx->inflight >= ctx->max_io_nr)
			return i ? (int)i : -EAGAIN;

		ret = resolve_obj_to_fd(&cb->obj, &file_fd);
		if (ret) {
			fprintf(stderr, "nds: resolve_obj failed %d\n", ret);
			return i ? (int)i : ret;
		}

		ret = nds_iocb_to_io_param(cb, file_fd, &io, &iov);
		if (ret) {
			fprintf(stderr, "nds: translate iocb failed %d\n", ret);
			return i ? (int)i : ret;
		}

		ret = ioctl(ctx->p2p_fd, IOCTL_RW_FILE, io);
		if (ret < 0) {
			ret = -errno;
			fprintf(stderr, "nds: rw ioctl failed %d\n", ret);
			free(io);
			return i ? (int)i : ret;
		}
		free(io);
		ctx->inflight++;
	}
	return (int)i;
}

int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout)
{
	struct p2p_getevents_param param = { 0 };
	struct p2p_io_event *raw = NULL;
	int ret;
	int i;

	if (!ctx || !events)
		return -EINVAL;
	if (nr == 0)
		return 0;
	if (min_nr > nr)
		return -EINVAL;

	raw = calloc(nr, sizeof(*raw));
	if (!raw)
		return -ENOMEM;

	param.min_nr = min_nr;
	param.max_nr = nr;
	if (timeout) {
		param.timeout_ns = (__s64)timeout->tv_sec * 1000000000LL +
				   timeout->tv_nsec;
	} else {
		param.timeout_ns = -1;
	}
	param.events = raw;

	ret = ioctl(ctx->p2p_fd, IOCTL_GET_IO_EVENTS, &param);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "nds: getevents ioctl failed %d\n", ret);
		free(raw);
		return ret;
	}
	for (i = 0; i < ret; i++) {
		memset(&events[i], 0, sizeof(events[i]));
		events[i].user_data = raw[i].user_data;
		events[i].res = raw[i].res;
		events[i].opcode = raw[i].op;
	}
	free(raw);
	if (ret > 0)
		ctx->inflight -= (unsigned int)ret;
	return ret;
}
