#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_p2p_api.h"

#define PATTERN_SIZE (1U << 20)
#define SEED_OFFSET (8U << 20)
#define SCRATCH_OFFSET (16U << 20)
#define RAW_BASE (24U << 20)
#define ALIGNMENT 4096UL

static int failures;
static const char *filesystem_device;
static uint64_t registered_handle;

static void pass(const char *name, long got, long want)
{
	if (got == want) {
		printf("api=c case=%s ret=%ld expected=%ld\n", name, got, want);
		return;
	}
	fprintf(stderr, "FAIL %s: got %ld, want %ld\n", name, got, want);
	failures++;
}

static void pass_file_case(const char *kind, const char *step, long got, long want)
{
	char name[80];

	snprintf(name, sizeof(name), "%s-%s", kind, step);
	pass(name, got, want);
}

static void fill_pattern(unsigned char *buffer, size_t length, uint64_t seed)
{
	size_t i;

	for (i = 0; i < length; i++)
		buffer[i] = (unsigned char)((i * 131 + (i >> 8) * 17 + seed * 29) & 0xff);
}

static int full_pread(int fd, void *buffer, size_t length, off_t offset)
{
	unsigned char *out = buffer;
	size_t done = 0;

	while (done < length) {
		ssize_t got = pread(fd, out + done, length - done, offset + (off_t)done);

		if (got < 0)
			return -errno;
		if (got == 0)
			return -EIO;
		done += (size_t)got;
	}
	return 0;
}

static int full_pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
	const unsigned char *in = buffer;
	size_t done = 0;

	while (done < length) {
		ssize_t put = pwrite(fd, in + done, length - done, offset + (off_t)done);

		if (put < 0)
			return -errno;
		if (put == 0)
			return -EIO;
		done += (size_t)put;
	}
	return 0;
}

static int odirect_write(const char *path, const unsigned char *data, size_t length, off_t offset)
{
	unsigned char *window;
	int fd;
	int err = 0;

	fd = open(path, O_WRONLY | O_DIRECT | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (posix_memalign((void **)&window, ALIGNMENT, length))
		err = -ENOMEM;
	if (!err) {
		memcpy(window, data, length);
		err = full_pwrite(fd, window, length, offset);
		free(window);
	}
	close(fd);
	return err;
}

static int verify_direct(const char *path, const unsigned char *expected, size_t length,
			 off_t offset, const char *name)
{
	unsigned char *window;
	int fd;
	int err;

	fd = open(path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (fd < 0) {
		pass(name, -errno, 0);
		return -errno;
	}
	if (posix_memalign((void **)&window, ALIGNMENT, length)) {
		close(fd);
		pass(name, -ENOMEM, 0);
		return -ENOMEM;
	}
	memset(window, 0, length);
	err = full_pread(fd, window, length, offset);
	close(fd);
	if (!err && memcmp(window, expected, length))
		err = -EILSEQ;
	free(window);
	pass(name, err, 0);
	return err;
}

struct extent_info {
	int mapped;
	uint64_t physical;
	uint64_t length;
	uint32_t flags;
};

static int query_fiemap(const char *path, off_t offset, off_t length, struct extent_info *out)
{
	struct {
		struct fiemap fm;
		struct fiemap_extent ex[8];
	} buf;
	int fd;
	int err;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	memset(&buf, 0, sizeof(buf));
	buf.fm.fm_start = (uint64_t)offset;
	buf.fm.fm_length = (uint64_t)length;
	buf.fm.fm_extent_count = 8;
	err = ioctl(fd, FS_IOC_FIEMAP, &buf.fm);
	close(fd);
	if (err)
		return -errno;
	if (out) {
		out->mapped = (int)buf.fm.fm_mapped_extents;
		if (buf.fm.fm_mapped_extents) {
			out->physical = buf.fm.fm_extents[0].fe_physical;
			out->length = buf.fm.fm_extents[0].fe_length;
			out->flags = buf.fm.fm_extents[0].fe_flags;
		}
	}
	return (int)buf.fm.fm_mapped_extents;
}

static int verify_unwritten(const char *path, size_t length)
{
	unsigned int max_extents = length / 512 + 1;
	struct fiemap *fm = calloc(1, sizeof(*fm) + max_extents * sizeof(struct fiemap_extent));
	size_t next = 0;
	unsigned int i;
	int fd;
	int err = -ENOMEM;

	if (!fm)
		return err;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		goto out;
	}
	fm->fm_length = length;
	fm->fm_extent_count = max_extents;
	err = ioctl(fd, FS_IOC_FIEMAP, fm) ? -errno : 0;
	close(fd);
	if (err)
		goto out;
	err = -ENODATA;
	for (i = 0; i < fm->fm_mapped_extents && next < length; i++) {
		const struct fiemap_extent *ex = &fm->fm_extents[i];

		if (ex->fe_logical != next || !ex->fe_length)
			goto out;
		if (!(ex->fe_flags & FIEMAP_EXTENT_UNWRITTEN)) {
			err = -EINVAL;
			goto out;
		}
		next += ex->fe_length < length - next ? ex->fe_length : length - next;
	}
	if (next == length)
		err = 0;
out:
	free(fm);
	return err;
}

/* Independent oracle: read each FIEMAP mapping directly from the filesystem
 * block device. Do not reuse the library extent preparation or XDS read path.
 */
static int verify_raw(const char *path, const unsigned char *expected, size_t length)
{
	unsigned int max_extents = length / 512 + 1;
	struct fiemap *fm = calloc(1, sizeof(*fm) + max_extents * sizeof(struct fiemap_extent));
	unsigned char *buffer = NULL;
	size_t next = 0;
	unsigned int i;
	int file_fd = -1, raw_fd = -1;
	int err = -ENOMEM;

	if (!fm || posix_memalign((void **)&buffer, ALIGNMENT, length))
		goto out;
	file_fd = open(path, O_RDONLY | O_CLOEXEC);
	raw_fd = open(filesystem_device, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (file_fd < 0 || raw_fd < 0) {
		err = -errno;
		goto out;
	}
	fm->fm_length = length;
	fm->fm_extent_count = max_extents;
	if (ioctl(file_fd, FS_IOC_FIEMAP, fm)) {
		err = -errno;
		goto out;
	}
	err = -ENODATA;
	for (i = 0; i < fm->fm_mapped_extents && next < length; i++) {
		struct fiemap_extent *ex = &fm->fm_extents[i];
		size_t chunk;

		if (ex->fe_logical != next || !ex->fe_length)
			goto out;
		chunk = ex->fe_length < length - next ? ex->fe_length : length - next;
		err = full_pread(raw_fd, buffer, chunk, (off_t)ex->fe_physical);
		if (err)
			goto out;
		if (memcmp(buffer, expected + next, chunk)) {
			err = -EILSEQ;
			goto out;
		}
		next += chunk;
		err = -ENODATA;
	}
	if (next == length)
		err = 0;
out:
	if (file_fd >= 0)
		close(file_fd);
	if (raw_fd >= 0)
		close(raw_fd);
	free(buffer);
	free(fm);
	pass("independent-raw-readback", err, 0);
	return err;
}

static int xds_rw(int dev_fd, unsigned int op, const char *path, unsigned long offset,
		  unsigned long cmb_va, unsigned long length)
{
	struct p2p_iov iov = { .addr = cmb_va, .size = length };
	struct io_parameter param = {
		.op = op,
		.file_name = path,
		.file_offset = offset,
		.iov = &iov,
		.iov_nr = 1,
		.host_pid = registered_handle ? 0 : getpid(),
		.flags = registered_handle ? P2P_IO_F_REGISTERED_MEM : 0,
		.mem_handle = registered_handle,
	};
	int err = rw_file(dev_fd, &param);

	if (err)
		return err;
	return drain_io(dev_fd);
}

static int seed_cmb(int dev_fd, const char *target, unsigned long cmb_va,
		    const unsigned char *pattern, size_t length)
{
	int err = odirect_write(target, pattern, length, SEED_OFFSET);

	if (err)
		return err;
	return xds_rw(dev_fd, P2P_IO_READ, target, SEED_OFFSET, cmb_va, length);
}

/* CMB is not host-visible: poison CMB and scratch, XDS-read the file into
 * CMB, XDS-write CMB to scratch, then O_DIRECT-compare scratch against the
 * expected pattern. Also records FIEMAP physical state of the file range.
 */
static void verify_via_xds(int dev_fd, const char *file_path, const char *scratch_dev,
			   unsigned long cmb_va, const unsigned char *pattern, size_t length,
			   const char *name)
{
	unsigned char *poison;
	struct extent_info ex = { 0 };
	int fiemap_err;
	int err;

	poison = malloc(length);
	if (!poison) {
		pass(name, -ENOMEM, 0);
		free(poison);
		return;
	}
	fill_pattern(poison, length, 0xDEAD);

	fiemap_err = query_fiemap(file_path, 0, (off_t)length, &ex);
	if (fiemap_err < 0) {
		pass(name, fiemap_err, 0);
		free(poison);
		return;
	}

	if (verify_raw(file_path, pattern, length)) {
		free(poison);
		return;
	}

	/* Poison CMB via seed read; poison scratch via O_DIRECT. */
	err = seed_cmb(dev_fd, scratch_dev, cmb_va, poison, length);
	if (!err)
		err = odirect_write(scratch_dev, poison, length, SCRATCH_OFFSET);
	if (!err) {
		err = xds_rw(dev_fd, P2P_IO_READ, file_path, 0, cmb_va, length);
		if (!err)
			err = xds_rw(dev_fd, P2P_IO_WRITE, scratch_dev, SCRATCH_OFFSET, cmb_va,
				     length);
	}
	if (!err)
		err = verify_direct(scratch_dev, pattern, length, SCRATCH_OFFSET, name);
	else
		pass(name, err, 0);

	pass("readback-fiemap-mapped", ex.mapped > 0 ? 0 : -ENODATA, 0);

	free(poison);
}

static void test_sparse_roundtrip(int dev_fd, const char *dir, const char *target,
				  unsigned long cmb_va, bool truncate_file)
{
	const char *kind = truncate_file ? "truncate" : "empty";
	char path[512], readback_case[80];
	struct stat st;
	unsigned char *pattern;
	int fd;
	int err;

	snprintf(path, sizeof(path), "%s/xds-%s-write.dat", dir, kind);
	unlink(path);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		pass_file_case(kind, "create", -errno, 0);
		return;
	}
	err = truncate_file && ftruncate(fd, PATTERN_SIZE) ? -errno : 0;
	if (truncate_file)
		pass_file_case(kind, "ftruncate", err, 0);
	if (!err) {
		err = fstat(fd, &st) ? -errno : 0;
		pass_file_case(kind, "initial-stat", err, 0);
		if (!err)
			pass_file_case(kind, "initial-size", st.st_size,
				       truncate_file ? PATTERN_SIZE : 0);
	}
	close(fd);
	if (err)
		goto unlink_file;
	pass_file_case(kind, "initial-hole", query_fiemap(path, 0, PATTERN_SIZE, NULL), 0);

	pattern = malloc(PATTERN_SIZE);
	if (!pattern) {
		pass_file_case(kind, "alloc", -ENOMEM, 0);
		goto unlink_file;
	}
	fill_pattern(pattern, PATTERN_SIZE, truncate_file ? 0x4444 : 0x1111);
	err = seed_cmb(dev_fd, target, cmb_va, pattern, PATTERN_SIZE);
	pass_file_case(kind, "seed-cmb", err, 0);
	if (!err) {
		err = xds_rw(dev_fd, P2P_IO_WRITE, path, 0, cmb_va, PATTERN_SIZE);
		pass_file_case(kind, "xds-write", err, 0);
		if (!err) {
			err = stat(path, &st) ? -errno : 0;
			pass_file_case(kind, "final-stat", err, 0);
			if (!err)
				pass_file_case(kind, "final-size", st.st_size, PATTERN_SIZE);
		}
	}
	if (!err) {
		snprintf(readback_case, sizeof(readback_case), "%s-xds-readback", kind);
		verify_via_xds(dev_fd, path, target, cmb_va, pattern, PATTERN_SIZE, readback_case);
		pass_file_case(kind, "final-unwritten", verify_unwritten(path, PATTERN_SIZE), 0);
	}

	free(pattern);
unlink_file:
	unlink(path);
}

static void test_unwritten_roundtrip(int dev_fd, const char *dir, const char *target,
				     unsigned long cmb_va)
{
	char path[512];
	unsigned char *pattern;
	int fd;
	int err;
	struct extent_info ex = { 0 };

	snprintf(path, sizeof(path), "%s/xds-unwritten.dat", dir);
	unlink(path);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		pass("unwritten-create", -errno, 0);
		return;
	}
	if (fallocate(fd, 0, 0, PATTERN_SIZE)) {
		pass("unwritten-fallocate", -errno, 0);
		close(fd);
		unlink(path);
		return;
	}
	close(fd);

	err = query_fiemap(path, 0, PATTERN_SIZE, &ex);
	pass("unwritten-pre-mapped", err > 0 ? 0 : -ENODATA, 0);
	pass("unwritten-before-write", !!(ex.flags & FIEMAP_EXTENT_UNWRITTEN), 1);

	pattern = malloc(PATTERN_SIZE);
	if (!pattern) {
		pass("unwritten-alloc", -ENOMEM, 0);
		unlink(path);
		return;
	}
	fill_pattern(pattern, PATTERN_SIZE, 0x2222);
	err = seed_cmb(dev_fd, target, cmb_va, pattern, PATTERN_SIZE);
	pass("unwritten-seed-cmb", err, 0);

	if (!err) {
		err = xds_rw(dev_fd, P2P_IO_WRITE, path, 0, cmb_va, PATTERN_SIZE);
		pass("unwritten-xds-write", err, 0);
	}
	if (!err)
		verify_via_xds(dev_fd, path, target, cmb_va, pattern, PATTERN_SIZE,
			       "unwritten-xds-readback");

	err = query_fiemap(path, 0, PATTERN_SIZE, &ex);
	pass("unwritten-post-mapped", err > 0 ? 0 : -ENODATA, 0);
	pass("unwritten-after-write", verify_unwritten(path, PATTERN_SIZE), 0);
	free(pattern);
	unlink(path);
}

static void test_fragmented_write(int dev_fd, const char *dir, const char *target,
				  unsigned long cmb_va)
{
	const unsigned long gap = 128U << 10;
	const unsigned long tail = 64U << 10;
	const unsigned long span = gap + tail;
	char path[512];
	unsigned char *pattern;
	unsigned char piece[4096];
	struct extent_info ex = { 0 };
	int fd;
	int err;

	snprintf(path, sizeof(path), "%s/xds-frag-write.dat", dir);
	unlink(path);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		pass("frag-create", -errno, 0);
		return;
	}
	memset(piece, 0xa5, sizeof(piece));
	err = full_pwrite(fd, piece, sizeof(piece), 0);
	if (!err)
		err = full_pwrite(fd, piece, sizeof(piece), gap);
	if (!err && fsync(fd))
		err = -errno;
	if (err) {
		pass("frag-seed", err, 0);
		close(fd);
		unlink(path);
		return;
	}
	close(fd);

	if (query_fiemap(path, 0, (off_t)span, &ex) < 2) {
		pass("frag-pre-extents", -ENODATA, 0);
		unlink(path);
		return;
	}
	pass("frag-pre-extents", ex.mapped >= 2 ? 0 : -ENODATA, 0);

	pattern = malloc(span);
	if (!pattern) {
		pass("frag-alloc", -ENOMEM, 0);
		unlink(path);
		return;
	}
	fill_pattern(pattern, span, 0x3333);
	err = seed_cmb(dev_fd, target, cmb_va, pattern, span);
	pass("frag-seed-cmb", err, 0);
	if (!err) {
		err = xds_rw(dev_fd, P2P_IO_WRITE, path, 0, cmb_va, span);
		pass("frag-multi-extent-write", err, 0);
	}
	if (!err)
		verify_via_xds(dev_fd, path, target, cmb_va, pattern, span, "frag-xds-readback");

	free(pattern);
	unlink(path);
}

/* Overwrite a nonzero range surrounded by initialized data. */
static void test_guarded_overwrite(int dev_fd, const char *dir, const char *scratch,
				   unsigned long cmb_va)
{
	unsigned char expected[4 * 4096];
	unsigned char payload[4096];
	char path[512];
	int fd, err;

	snprintf(path, sizeof(path), "%s/xds-guarded.dat", dir);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		pass("guard-create", -errno, 0);
		return;
	}
	close(fd);
	fill_pattern(expected, sizeof(expected), 0x55);
	fill_pattern(payload, sizeof(payload), 0x66);
	err = odirect_write(path, expected, sizeof(expected), 0);
	pass("guard-initialize", err, 0);
	if (!err)
		err = seed_cmb(dev_fd, scratch, cmb_va, payload, sizeof(payload));
	pass("guard-seed", err, 0);
	if (!err)
		err = xds_rw(dev_fd, P2P_IO_WRITE, path, 4096, cmb_va, sizeof(payload));
	pass("nonzero-overwrite", err, 0);
	if (!err) {
		memcpy(expected + 4096, payload, sizeof(payload));
		verify_raw(path, expected, sizeof(expected));
	}
	unlink(path);
}

static void test_library_errors(const char *dir)
{
	char path[512];
	struct p2p_iov iov = { .addr = 0, .size = 512 };
	struct io_parameter param = {};
	int fd;

	snprintf(path, sizeof(path), "%s/xds-lib-errors.dat", dir);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0) {
		pass("lib-error-create", -errno, 0);
		return;
	}
	if (ftruncate(fd, 8192)) {
		pass("lib-error-truncate", -errno, 0);
		close(fd);
		unlink(path);
		return;
	}
	close(fd);

	param.op = P2P_IO_READ;
	param.file_name = path;
	param.file_offset = 4096;
	param.iov = &iov;
	param.iov_nr = 1;
	param.host_pid = getpid();
	pass("read-hole-enodata", rw_file(-1, &param), -ENODATA);

	/* 256 is not sector-aligned (512 is). */
	param.file_offset = 256;
	pass("read-unaligned-offset", rw_file(-1, &param), -EINVAL);

	param.file_offset = 0;
	iov.size = 513;
	pass("read-unaligned-size", rw_file(-1, &param), -EINVAL);

	iov.size = 512;
	param.op = P2P_IO_WRITE;
	param.host_pid = -1;
	pass("write-negative-host-pid", rw_file(-1, &param), -EINVAL);

	unlink(path);
}

static void test_raw_bdev_regression(int dev_fd, const char *target, unsigned long cmb_va)
{
	unsigned char pattern[4096];
	unsigned char expected[12288];
	unsigned long destination = RAW_BASE + (8U << 20);
	struct p2p_iov iov = { .addr = cmb_va, .size = sizeof(pattern) };
	struct fiemap_extent extents[2] = {
		{ .fe_physical = destination + 8192, .fe_length = 2048 },
		{ .fe_physical = destination, .fe_length = 2048 },
	};
	struct p2p_io_param io = {
		.op = P2P_IO_WRITE,
		.host_pid = getpid(),
		.iov = (uint64_t)(uintptr_t)&iov,
		.iov_nr = 1,
		.extents = (uint64_t)(uintptr_t)extents,
		.ext_nr = 2,
	};
	struct p2p_io_batch_param batch = { .items = (uint64_t)(uintptr_t)&io, .nr = 1 };
	int fd;
	int err;

	fill_pattern(pattern, sizeof(pattern), 0x4444);
	err = odirect_write(target, pattern, sizeof(pattern), RAW_BASE);
	pass("raw-bdev-host-seed", err, 0);
	if (err)
		return;
	err = xds_rw(dev_fd, P2P_IO_READ, target, RAW_BASE, cmb_va, 4096);
	pass("raw-bdev-xds-read", err, 0);
	if (err)
		return;
	err = xds_rw(dev_fd, P2P_IO_WRITE, target, RAW_BASE + (8U << 20), cmb_va, 4096);
	pass("raw-bdev-write", err, 0);
	if (!err)
		verify_direct(target, pattern, 4096, RAW_BASE + (8U << 20), "raw-bdev-verify");
	if (err)
		return;

	/* Scatter source halves in reverse disk order, preserving the gap and tail. */
	memset(expected, 0xa5, sizeof(expected));
	err = odirect_write(target, expected, sizeof(expected), destination);
	pass("raw-multi-extent-guards", err, 0);
	if (err)
		return;
	fd = open(target, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		pass("raw-multi-extent-open", -errno, 0);
		return;
	}
	io.file_fd = fd;
	err = ioctl(dev_fd, IOCTL_SUBMIT_IO, &batch);
	pass("raw-multi-extent-submit", err < 0 ? -errno : err, 1);
	if (err == 1) {
		err = drain_io(dev_fd);
		pass("raw-multi-extent-drain", err, 0);
		if (!err) {
			memcpy(expected + 8192, pattern, 2048);
			memcpy(expected, pattern + 2048, 2048);
			verify_direct(target, expected, sizeof(expected), destination,
				      "raw-multi-extent-readback");
		}
	}
	close(fd);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "scratch", required_argument, NULL, 's' },
		{ "directory", required_argument, NULL, 'd' },
		{ "cmb-va", required_argument, NULL, 'a' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topology = NULL;
	const char *scratch = NULL;
	const char *directory = NULL;
	unsigned long cmb_va = 0;
	int dev_fd = -1;
	int option;

	while ((option = getopt_long(argc, argv, "t:s:d:a:h", options, NULL)) != -1) {
		switch (option) {
		case 't':
			topology = optarg;
			break;
		case 's':
			scratch = optarg;
			break;
		case 'd':
			directory = optarg;
			break;
		case 'a':
			cmb_va = strtoul(optarg, NULL, 0);
			break;
		default:
			fprintf(stderr,
				"Usage: %s --topology <fs-bdev> --scratch "
				"<raw-bdev> --directory <ext4-dir> "
				"--cmb-va <bytes>\n",
				argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (!topology || !scratch || !directory || !cmb_va || optind != argc)
		return EXIT_FAILURE;

	{
		struct stat fs_st, scratch_st, dir_st;

		if (stat(topology, &fs_st) || stat(scratch, &scratch_st) ||
		    stat(directory, &dir_st))
			return EXIT_FAILURE;
		if (!S_ISBLK(fs_st.st_mode) || !S_ISBLK(scratch_st.st_mode) ||
		    fs_st.st_rdev == scratch_st.st_rdev || fs_st.st_rdev != dir_st.st_dev) {
			fprintf(stderr,
				"require directory on topology and a distinct scratch device\n");
			return EXIT_FAILURE;
		}
	}
	filesystem_device = topology;
	dev_fd = new_p2p_fd();
	if (dev_fd < 0) {
		fprintf(stderr, "new_p2p_fd failed: %d\n", dev_fd);
		return EXIT_FAILURE;
	}
	if (add_topo(dev_fd, topology)) {
		fprintf(stderr, "add_topo topology failed\n");
		close_p2p_fd(dev_fd);
		return EXIT_FAILURE;
	}
	if (add_topo(dev_fd, scratch)) {
		fprintf(stderr, "add_topo scratch failed\n");
		close_p2p_fd(dev_fd);
		return EXIT_FAILURE;
	}

	test_sparse_roundtrip(dev_fd, directory, scratch, cmb_va, false);
	test_sparse_roundtrip(dev_fd, directory, scratch, cmb_va, true);
	test_unwritten_roundtrip(dev_fd, directory, scratch, cmb_va);
	test_fragmented_write(dev_fd, directory, scratch, cmb_va);
	test_guarded_overwrite(dev_fd, directory, scratch, cmb_va);
	test_library_errors(directory);
	test_raw_bdev_regression(dev_fd, scratch, cmb_va);

	{
		struct p2p_mem_register_param reg = { .addr = cmb_va, .size = PATTERN_SIZE };
		struct p2p_mem_unregister_param unreg = { 0 };
		int err = register_mem(dev_fd, &reg);

		pass("regular-register-memory", err, 0);
		if (!err) {
			registered_handle = reg.mem_handle;
			test_sparse_roundtrip(dev_fd, directory, scratch, cmb_va, false);
			test_sparse_roundtrip(dev_fd, directory, scratch, cmb_va, true);
			test_unwritten_roundtrip(dev_fd, directory, scratch, cmb_va);
			test_guarded_overwrite(dev_fd, directory, scratch, cmb_va);
			unreg.mem_handle = registered_handle;
			registered_handle = 0;
			pass("regular-unregister-memory", unregister_mem(dev_fd, &unreg), 0);
		}
	}
	close_p2p_fd(dev_fd);
	if (failures) {
		fprintf(stderr, "file_raw_write_test: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
