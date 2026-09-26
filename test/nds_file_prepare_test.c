/* Real allocation with mocked FIEMAP/submission; no XDS device or payload I/O. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nds_api.h"
#include "p2p_dev_uapi.h"

static int submit_result = 1;
static unsigned int submit_calls;
static unsigned int submitted_items;
static unsigned int fiemap_calls;
static int target_fd[2];

int __wrap_p2p_open_dev(void)
{
	return open("/dev/null", O_RDWR | O_CLOEXEC);
}

static off_t file_size(int fd)
{
	struct stat st;

	assert(fstat(fd, &st) == 0);
	return st.st_size;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	/* Drain has no variadic argument. */
	if (request == IOCTL_DRAIN_IO)
		return 0;
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (request == FS_IOC_FIEMAP) {
		struct fiemap *fm = arg;
		struct fiemap_extent *extent = fm->fm_extents;

		fiemap_calls++;
		assert(fd == target_fd[0] || fd == target_fd[1]);
		/* Allocation must have happened before the mapping request. */
		assert(file_size(fd) >= (off_t)(fm->fm_start + fm->fm_length));
		assert(fm->fm_extent_count >= 1);
		memset(extent, 0, sizeof(*extent));
		extent->fe_logical = fm->fm_start;
		extent->fe_physical = 1U << 20;
		extent->fe_length = fm->fm_length;
		extent->fe_flags = FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_UNWRITTEN;
		fm->fm_mapped_extents = 1;
		return 0;
	}
	assert(request == IOCTL_SUBMIT_IO);
	{
		struct p2p_io_batch_param *batch = arg;
		struct p2p_io_param *items = (void *)(uintptr_t)batch->items;
		unsigned int i;

		submit_calls++;
		submitted_items = batch->nr;
		for (i = 0; i < batch->nr; i++) {
			struct fiemap_extent *extent = (void *)(uintptr_t)items[i].extents;

			assert(items[i].op == P2P_IO_WRITE);
			assert(items[i].file_fd == target_fd[i]);
			assert(items[i].ext_nr == 1);
			assert(extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN);
			assert(file_size(items[i].file_fd) >=
			       (off_t)(extent->fe_logical + extent->fe_length));
		}
	}
	if (submit_result < 0) {
		errno = -submit_result;
		return -1;
	}
	return submit_result;
}

static void reset_files(void)
{
	assert(ftruncate(target_fd[0], 0) == 0);
	assert(ftruncate(target_fd[1], 0) == 0);
	submit_calls = 0;
	submitted_items = 0;
	fiemap_calls = 0;
}

int main(void)
{
	struct nds_init_param init = { 0 };
	struct nds_io_ctx_param params = { .max_io_cnt = 2 };
	struct nds_io_ctx *ctx;
	struct nds_io_vec iov = { .buf_addr = 4096, .buf_len = 4096 };
	struct nds_io_cb cb[2] = { 0 };
	char first[] = "/tmp/xds-nds-first.XXXXXX";
	char second[] = "/tmp/xds-nds-second.XXXXXX";
	unsigned int i;

	target_fd[0] = mkstemp(first);
	target_fd[1] = mkstemp(second);
	assert(target_fd[0] >= 0 && target_fd[1] >= 0);
	assert(unlink(first) == 0 && unlink(second) == 0);
	assert(nds_init(&init) == 0);
	assert(nds_io_new_ctx(&params, &ctx) == 0);
	for (i = 0; i < 2; i++) {
		cb[i].opcode = NDS_IO_OP_PWRITE;
		cb[i].obj.fd = target_fd[i];
		cb[i].iov = &iov;
		cb[i].iov_cnt = 1;
		cb[i].offset = i * 8192;
	}

	/* A rejected tail still had its allocation prepared before submission. */
	reset_files();
	assert(nds_io_submit(ctx, 2, cb) == 1);
	assert(submit_calls == 1 && submitted_items == 2 && fiemap_calls == 2);
	assert(file_size(target_fd[0]) == 4096 && file_size(target_fd[1]) == 12288);
	puts("nds file prepare: partial acceptance preserves both allocations");

	reset_files();
	submit_result = -EAGAIN;
	assert(nds_io_submit(ctx, 2, cb) == -EAGAIN);
	assert(submit_calls == 1 && fiemap_calls == 2);
	assert(file_size(target_fd[0]) == 4096 && file_size(target_fd[1]) == 12288);
	puts("nds file prepare: rejected batch preserves preparation side effects");

	/* A library-invalid tail is never allocated or sent to the kernel. */
	reset_files();
	submit_result = 1;
	cb[1].reserved[0] = 1;
	assert(nds_io_submit(ctx, 2, cb) == 1);
	assert(submit_calls == 1 && submitted_items == 1 && fiemap_calls == 1);
	assert(file_size(target_fd[0]) == 4096 && file_size(target_fd[1]) == 0);
	puts("nds file prepare: invalid tail stops preparation at valid prefix");
	cb[1].reserved[0] = 0;

	reset_files();
	iov.buf_len = 0;
	assert(nds_io_submit(ctx, 2, cb) == -EINVAL);
	assert(submit_calls == 0 && fiemap_calls == 0);
	assert(file_size(target_fd[0]) == 0 && file_size(target_fd[1]) == 0);
	iov.buf_len = 4096;
	iov.reserved = 1;
	assert(nds_io_submit(ctx, 2, cb) == -EINVAL);
	assert(submit_calls == 0 && fiemap_calls == 0);
	assert(file_size(target_fd[0]) == 0 && file_size(target_fd[1]) == 0);
	iov.reserved = 0;
	puts("nds file prepare: empty and reserved IOVs rejected before allocation");

	/* The kernel rejects misaligned source addresses after preparation. */
	reset_files();
	submit_result = -EINVAL;
	iov.buf_addr = 1;
	assert(nds_io_submit(ctx, 2, cb) == -EINVAL);
	assert(submit_calls == 1 && submitted_items == 2 && fiemap_calls == 2);
	assert(file_size(target_fd[0]) == 4096 && file_size(target_fd[1]) == 12288);
	puts("nds file prepare: source validation deferred to kernel submission");

	assert(nds_io_destroy_ctx(ctx) == 0);
	assert(nds_exit() == 0);
	assert(close(target_fd[0]) == 0 && close(target_fd[1]) == 0);
	puts("nds_file_prepare_test: all cases passed");
	return 0;
}
