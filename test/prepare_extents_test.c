/* Host-only p2p_prepare_io_extents contract tests.
 * FS_IOC_FIEMAP responses are programmed through an ioctl wrapper so flag,
 * trimming and coverage cases do not need a prepared VM. fallocate runs
 * against a real file. No block I/O occurs.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "p2p_common.h"

#define MAX_FAKE_EXTS 8U

struct fiemap_program {
	int active;
	int fail_errno;
	unsigned int mapped;
	struct fiemap_extent exts[MAX_FAKE_EXTS];
};

static struct fiemap_program program;
static int fallocate_calls;
static int fallocate_errno;
static long fallocate_mode;
static off_t fallocate_offset;
static off_t fallocate_len;
static int failures;
static char temp_path[] = "prepare_extents_test.XXXXXX";
static int temp_fd = -1;
static int real_fiemap_supported = 1;

int __real_ioctl(int fd, unsigned long request, ...);
int __real_fallocate(int fd, int mode, off_t offset, off_t len);
int __real_fcntl(int fd, int cmd, ...);

static void pass(const char *name, long got, long want)
{
	if (got == want) {
		printf("api=c case=%s ret=%ld expected=%ld\n", name, got, want);
		return;
	}
	fprintf(stderr, "FAIL %s: got %ld, want %ld\n", name, got, want);
	failures++;
}

int __wrap_fallocate(int fd, int mode, off_t offset, off_t len)
{
	fallocate_calls++;
	fallocate_mode = mode;
	fallocate_offset = offset;
	fallocate_len = len;
	if (fallocate_errno) {
		errno = fallocate_errno;
		return -1;
	}
	return __real_fallocate(fd, mode, offset, len);
}

int __wrap_fcntl(int fd, int cmd, ...)
{
	return __real_fcntl(fd, cmd);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	unsigned int i;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (request == FS_IOC_FIEMAP && program.active) {
		struct fiemap *fm = arg;
		unsigned int copy;

		if (program.fail_errno) {
			errno = program.fail_errno;
			return -1;
		}
		/* Report a mapped count larger than max_num so the library's
		 * overflow check can fire without writing past the buffer.
		 */
		copy = program.mapped;
		if (copy > fm->fm_extent_count)
			copy = fm->fm_extent_count;
		for (i = 0; i < copy; i++)
			fm->fm_extents[i] = program.exts[i];
		fm->fm_mapped_extents = program.mapped;
		return 0;
	}
	return __real_ioctl(fd, request, arg);
}

static void reset_program(void)
{
	memset(&program, 0, sizeof(program));
	fallocate_calls = 0;
	fallocate_errno = 0;
	fallocate_mode = 0;
	fallocate_offset = 0;
	fallocate_len = 0;
}

static void set_extent(struct fiemap_extent *extent, uint64_t logical, uint64_t physical,
		       uint64_t length, uint32_t flags)
{
	memset(extent, 0, sizeof(*extent));
	extent->fe_logical = logical;
	extent->fe_physical = physical;
	extent->fe_length = length;
	extent->fe_flags = flags;
}

static int prepare(unsigned int op, unsigned long offset, unsigned long size, struct fiemap **exts,
		   unsigned int *ext_num, unsigned long long *total)
{
	struct stat st;

	if (fstat(temp_fd, &st))
		return -errno;
	return p2p_prepare_io_extents(temp_fd, &st, op, offset, size, exts, ext_num, total);
}

static int prepare_expect(const char *name, unsigned int op, unsigned long offset,
			  unsigned long size, int want)
{
	struct fiemap *exts = NULL;
	unsigned int ext_num = 0;
	unsigned long long total = 0;
	int got = prepare(op, offset, size, &exts, &ext_num, &total);

	free(exts);
	pass(name, got, want);
	return got;
}

static int probe_real_fiemap(void)
{
	struct {
		struct fiemap fm;
		struct fiemap_extent ex[1];
	} buf;
	int err;

	memset(&buf, 0, sizeof(buf));
	buf.fm.fm_start = 0;
	buf.fm.fm_length = 4096;
	buf.fm.fm_extent_count = 1;
	errno = 0;
	if (ioctl(temp_fd, FS_IOC_FIEMAP, &buf.fm) == 0)
		return 1;
	err = errno;
	return err != EOPNOTSUPP && err != ENOTTY && err != EINVAL;
}

int main(void)
{
	struct fiemap *exts = NULL;
	unsigned int ext_num = 0;
	unsigned long long total = 0;
	unsigned char pattern[4096];
	int got;

	temp_fd = mkstemp(temp_path);
	if (temp_fd < 0) {
		perror("mkstemp");
		return EXIT_FAILURE;
	}
	if (ftruncate(temp_fd, 4 << 20)) {
		perror("ftruncate");
		return EXIT_FAILURE;
	}

	reset_program();
	prepare_expect("zero-size", P2P_IO_READ, 0, 0, -EINVAL);
	prepare_expect("unaligned-offset", P2P_IO_READ, 256, 4096, -EINVAL);
	prepare_expect("unaligned-size", P2P_IO_READ, 0, 100, -EINVAL);
	prepare_expect("offset-overflow", P2P_IO_READ, ULONG_MAX - 511UL, 512, -EOVERFLOW);
	prepare_expect("signed-overflow", P2P_IO_WRITE, 1UL << 63, 4096, -EOVERFLOW);

	reset_program();
	got = prepare(P2P_IO_WRITE, 0, 4096, &exts, &ext_num, &total);
	pass("write-fallocate-mode0", got, 0);
	if (got == 0) {
		pass("write-fallocate-called", fallocate_calls, 1);
		pass("write-fallocate-flags", fallocate_mode, 0);
		pass("write-fallocate-offset", (long)fallocate_offset, 0);
		pass("write-fallocate-length", (long)fallocate_len, 4096);
	}
	free(exts);
	exts = NULL;

	reset_program();
	fallocate_errno = ENOSPC;
	prepare_expect("write-fallocate-fail", P2P_IO_WRITE, 0, 4096, -ENOSPC);

	reset_program();
	got = prepare(P2P_IO_READ, 0, 4096, &exts, &ext_num, &total);
	pass("read-skips-fallocate", (got == 0 && fallocate_calls == 0) ? 0L : -1L, 0);
	free(exts);
	exts = NULL;

	reset_program();
	{
		struct stat st;
		int readonly = open(temp_path, O_RDONLY | O_CLOEXEC);

		if (readonly < 0 || fstat(temp_fd, &st))
			return EXIT_FAILURE;
		got = p2p_prepare_io_extents(readonly, &st, P2P_IO_WRITE, 0, 4096, &exts, &ext_num,
					     &total);
		pass("readonly-write", got, -EBADF);
		pass("readonly-fallocate-rejected", fallocate_calls, 1);
		close(readonly);
		reset_program();
		st.st_mode = S_IFBLK;
		got = p2p_prepare_io_extents(-1, &st, P2P_IO_WRITE, 4096, 4096, &exts, &ext_num,
					     &total);
		pass("bdev-skips-allocation", got, 0);
		pass("bdev-no-allocation", fallocate_calls, 0);
		if (!got) {
			pass("bdev-physical-offset", exts->fm_extents[0].fe_physical, 4096);
			pass("bdev-single-extent", ext_num, 1);
		}
		free(exts);
		exts = NULL;
	}

	reset_program();
	program.active = 1;
	program.mapped = 1;
	set_extent(&program.exts[0], 0, 4096, 4096, FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_UNWRITTEN);
	prepare_expect("unwritten-read", P2P_IO_READ, 0, 4096, 0);
	prepare_expect("unwritten-write", P2P_IO_WRITE, 0, 4096, 0);
	pass("unwritten-write-fallocates", fallocate_calls, 1);

	reset_program();
	program.active = 1;
	program.mapped = 1;
	set_extent(&program.exts[0], 0, 4096, 4096, FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_SHARED);
	prepare_expect("shared-read-ok", P2P_IO_READ, 0, 4096, 0);
	prepare_expect("shared-write-rejected", P2P_IO_WRITE, 0, 4096, -EOPNOTSUPP);

	reset_program();
	program.active = 1;
	program.mapped = 1;
	set_extent(&program.exts[0], 0, 4096, 4096, FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_DELALLOC);
	prepare_expect("delalloc-rejected", P2P_IO_READ, 0, 4096, -EOPNOTSUPP);

	reset_program();
	program.active = 1;
	program.mapped = 2;
	set_extent(&program.exts[0], 0, 4096, 4096, FIEMAP_EXTENT_UNWRITTEN);
	set_extent(&program.exts[1], 8192, 16384, 4096, FIEMAP_EXTENT_LAST);
	got = prepare(P2P_IO_READ, 0, 12288, &exts, &ext_num, &total);
	pass("logical-gap-prepared", got, 0);
	if (!got) {
		pass("logical-gap-ext-count", ext_num, 2);
		pass("logical-gap-total", total, 8192);
		pass("logical-gap-preserved", exts->fm_extents[1].fe_logical, 8192);
	}
	free(exts);
	exts = NULL;

	reset_program();
	program.active = 1;
	program.mapped = 1;
	set_extent(&program.exts[0], 0, 4096, 4096, FIEMAP_EXTENT_LAST);
	got = prepare(P2P_IO_READ, 0, 8192, &exts, &ext_num, &total);
	pass("trailing-hole-prepared", got, 0);
	if (!got)
		pass("trailing-hole-short-coverage", total, 4096);
	free(exts);
	exts = NULL;

	reset_program();
	program.active = 1;
	program.mapped = 1;
	set_extent(&program.exts[0], 0, 4097, 4096, FIEMAP_EXTENT_LAST);
	prepare_expect("unaligned-physical-deferred", P2P_IO_READ, 0, 4096, 0);

	reset_program();
	program.active = 1;
	program.fail_errno = EIO;
	prepare_expect("fiemap-io-error", P2P_IO_READ, 0, 4096, -EIO);

	reset_program();
	program.active = 1;
	program.mapped = 3;
	set_extent(&program.exts[0], 0, 4096, 4096, 0);
	set_extent(&program.exts[1], 4096, 12288, 4096, 0);
	set_extent(&program.exts[2], 8192, 20480, 4096, FIEMAP_EXTENT_LAST);
	got = prepare(P2P_IO_WRITE, 0, 12288, &exts, &ext_num, &total);
	pass("fragmented-contiguous-write", got, 0);
	if (got == 0) {
		pass("fragmented-ext-count", ext_num, 3);
		pass("fragmented-total", (long)total, 12288);
	}
	free(exts);
	exts = NULL;

	reset_program();
	program.active = 1;
	program.mapped = 4;
	prepare_expect("mapped-exceeds-buffer", P2P_IO_READ, 0, 4096, -EINVAL);

	/* Real filesystem FIEMAP: hole on read, fallocate closes it on write. */
	reset_program();
	program.active = 0;
	real_fiemap_supported = probe_real_fiemap();
	memset(pattern, 0x5a, sizeof(pattern));
	if (pwrite(temp_fd, pattern, sizeof(pattern), 0) != 4096 ||
	    pwrite(temp_fd, pattern, sizeof(pattern), 2 << 20) != 4096) {
		perror("pwrite");
		return EXIT_FAILURE;
	}
	if (fsync(temp_fd)) {
		perror("fsync");
		return EXIT_FAILURE;
	}

	if (!real_fiemap_supported) {
		printf("api=c case=real-fiemap-skip ret=0 expected=0\n");
	} else {
		got = prepare(P2P_IO_READ, 0, (2 << 20) + 4096, &exts, &ext_num, &total);
		pass("real-hole-read-prepared", got, 0);
		if (!got)
			pass("real-hole-read-short-coverage", total < (2 << 20) + 4096, 1);
		free(exts);
		exts = NULL;

		got = prepare(P2P_IO_WRITE, 0, (2 << 20) + 4096, &exts, &ext_num, &total);
		pass("real-hole-write-fallocates", got, 0);
		if (got == 0) {
			pass("real-hole-write-ext-total", (long)total, (2 << 20) + 4096);
			pass("real-hole-write-multi-ext", ext_num >= 2 ? 0L : -1L, 0);
		}
		free(exts);
		exts = NULL;
	}

	close(temp_fd);
	temp_fd = -1;
	unlink(temp_path);

	if (failures) {
		fprintf(stderr, "prepare_extents_test: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("prepare_extents_test: all cases passed\n");
	return EXIT_SUCCESS;
}
