#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "file_p2p_api.h"

#define TEST_TOO_LARGE_IOV_SIZE ((2U << 30) + 512U)
#define TEST_TOO_LARGE_EXTENT_SIZE (1ULL << 41)

_Static_assert(sizeof(struct p2p_iov) == 16, "p2p_iov must remain a 16-byte UAPI record");
_Static_assert(offsetof(struct p2p_iov, reserved) == 12,
	       "p2p_iov reserved field must occupy the old padding");
_Static_assert(sizeof(struct p2p_io_param) == 80, "p2p_io_param includes reserved[3]");
_Static_assert(sizeof(struct p2p_io_event) == 48,
	       "p2p_io_event matches nds_io_event: user_data + res + reserved[4]");
_Static_assert(sizeof(((struct p2p_io_param *)0)->iov) == sizeof(uint64_t),
	       "p2p_io_param iov address must be fixed-width");
_Static_assert(sizeof(((struct p2p_getevents_param *)0)->events) == sizeof(uint64_t),
	       "p2p_getevents_param events address must be fixed-width");
_Static_assert(sizeof(struct p2p_io_batch_param) == 16,
	       "p2p_io_batch_param must remain a 16-byte UAPI record");
_Static_assert(offsetof(struct p2p_io_batch_param, items) == 0,
	       "p2p_io_batch_param items must remain the first field");

static int expect(const char *name, int actual, int expected)
{
	if (actual == expected) {
		printf("api=c case=%s ret=%d expected=%d\n", name, actual, expected);
		return 0;
	}
	fprintf(stderr, "%s returned %d, expected %d\n", name, actual, expected);
	return -EINVAL;
}

static int issue(int dev_fd, struct p2p_io_param *param)
{
	struct p2p_io_batch_param batch = {
		.nr = 1,
		.items = (uint64_t)(uintptr_t)param,
	};
	int ret = ioctl(dev_fd, IOCTL_SUBMIT_IO, &batch);

	if (ret < 0)
		return -errno;
	return ret == 1 ? 0 : -EIO;
}

static int issue_batch(int dev_fd, struct p2p_io_batch_param *batch)
{
	int ret = ioctl(dev_fd, IOCTL_SUBMIT_IO, batch);

	return ret < 0 ? -errno : ret;
}

static int expect_no_event(int dev_fd, const char *name)
{
	struct p2p_io_event event = { 0 };
	struct p2p_getevents_param get = {
		.min_nr = 1,
		.max_nr = 1,
		.timeout_ns = 0,
		.events = (uint64_t)(uintptr_t)&event,
	};
	int ret = ioctl(dev_fd, IOCTL_GET_IO_EVENTS, &get);

	if (ret < 0)
		return expect(name, -errno, 0);
	return expect(name, ret, 0);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "target", required_argument, NULL, 't' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct p2p_io_param *items;
	struct p2p_io_batch_param batch = { 0 };
	struct fiemap_extent extent = { 0 };
	struct fiemap_extent extents[2] = { 0 };
	struct io_parameter userspace = {};
	struct p2p_iov iov = {
		.addr = 0,
		.size = 512,
	};
	const char *target = NULL;
	char regular_path[] = "uapi-rw-regular.XXXXXX";
	uint64_t capacity;
	int regular_fd = -1;
	int write_fd = -1;
	int read_fd = -1;
	int dev_fd = -1;
	int option;
	int err = 0;

	while ((option = getopt_long(argc, argv, "t:h", options, NULL)) != -1) {
		switch (option) {
		case 't':
			target = optarg;
			break;
		case 'h':
			printf("Usage: %s --target <block-device>\n", argv[0]);
			return EXIT_SUCCESS;
		default:
			return EXIT_FAILURE;
		}
	}
	if (!target || optind != argc)
		return EXIT_FAILURE;

	items = calloc(2, sizeof(*items));
	if (!items)
		return EXIT_FAILURE;

	regular_fd = mkstemp(regular_path);
	if (regular_fd < 0) {
		err = -errno;
		goto out;
	}
	/* Empty file: the userspace write path fallocates before FIEMAP. */
	if (ftruncate(regular_fd, 0)) {
		err = -errno;
		goto out;
	}
	read_fd = open(target, O_RDONLY | O_CLOEXEC);
	if (read_fd < 0) {
		err = -errno;
		goto out;
	}
	write_fd = open(target, O_WRONLY | O_CLOEXEC);
	if (write_fd < 0) {
		err = -errno;
		goto out;
	}
	if (ioctl(read_fd, BLKGETSIZE64, &capacity)) {
		err = -errno;
		goto out;
	}

	dev_fd = new_p2p_fd();
	if (dev_fd < 0) {
		err = dev_fd;
		goto out;
	}
	items[0].op = P2P_IO_WRITE;
	items[0].host_pid = getpid();
	items[0].file_fd = write_fd;
	items[0].iov = (uint64_t)(uintptr_t)&iov;
	items[0].iov_nr = 1;
	items[0].ext_nr = 1;
	extent.fe_logical = 0;
	extent.fe_physical = capacity - 4096;
	extent.fe_length = 512;
	items[0].extents = (uint64_t)(uintptr_t)&extent;

	batch.items = (uint64_t)(uintptr_t)items;
	err = expect("batch zero count", issue_batch(dev_fd, &batch), -EINVAL);
	if (err)
		goto out;
	batch.nr = 1;
	batch.reserved = 1;
	err = expect("batch reserved field", issue_batch(dev_fd, &batch), -EINVAL);
	if (err)
		goto out;
	batch.reserved = 0;
	batch.items = 0;
	err = expect("batch null items", issue_batch(dev_fd, &batch), -EFAULT);
	if (err)
		goto out;
	batch.items = 1;
	err = expect("batch bad items pointer", issue_batch(dev_fd, &batch), -EFAULT);
	if (err)
		goto out;
	batch.items = (uint64_t)(uintptr_t)items;
	batch.nr = P2P_MAX_IO_NR + 1;
	err = expect("batch excessive count", issue_batch(dev_fd, &batch), -EINVAL);
	if (err)
		goto out;
	batch.nr = 1;

	items[0].op = P2P_IO_WRITE + 1;
	err = expect("unknown operation", issue(dev_fd, &items[0]), -EINVAL);
	if (err)
		goto out;
	err = expect("batch item failure", issue_batch(dev_fd, &batch), -EINVAL);
	if (err)
		goto out;
	items[0].op = P2P_IO_WRITE;
	items[0].flags = P2P_IO_F_MASK << 1;
	err = expect("unknown flags", issue(dev_fd, &items[0]), -EOPNOTSUPP);
	if (err)
		goto out;
	items[0].flags = 0;
	items[0].reserved[0] = 1;
	err = expect("IO reserved field", issue(dev_fd, &items[0]), -EINVAL);
	if (err)
		goto out;
	items[0].reserved[0] = 0;
	iov.reserved = 1;
	err = expect("IOV reserved field", issue(dev_fd, &items[0]), -EINVAL);
	if (err)
		goto out;
	iov.reserved = 0;
	iov.size = TEST_TOO_LARGE_IOV_SIZE;
	err = expect("IOV exceeds 2 GiB", issue(dev_fd, &items[0]), -EINVAL);
	if (err)
		goto out;
	iov.size = 512;
	extent.fe_length = TEST_TOO_LARGE_EXTENT_SIZE;
	err = expect("extent reaches 2 TiB", issue(dev_fd, &items[0]), -EINVAL);
	if (err)
		goto out;
	extent.fe_length = 512;

	/* Regular-file raw writes are no longer EOPNOTSUPP. Without a
	 * registered topology for the file's backing device they fail later
	 * with ENODEV, which proves the old gate is gone. No data moves.
	 */
	items[0].file_fd = regular_fd;
	err = expect("regular file write without topology", issue(dev_fd, &items[0]), -ENODEV);
	if (err)
		goto out;

	{
		int readonly = open(regular_path, O_RDONLY | O_CLOEXEC);

		if (readonly < 0) {
			err = -errno;
			goto out;
		}
		items[0].file_fd = readonly;
		err = expect("read-only regular fd", issue(dev_fd, &items[0]), -EBADF);
		close(readonly);
		if (err)
			goto out;
	}

	items[0].file_fd = read_fd;
	err = expect("read-only block fd", issue(dev_fd, &items[0]), -EBADF);
	if (err)
		goto out;

	/* SHARED and unsupported flags are rejected before any transfer. */
	items[0].file_fd = write_fd;
	extent.fe_flags = FIEMAP_EXTENT_SHARED;
	err = expect("shared write extent", issue(dev_fd, &items[0]), -EOPNOTSUPP);
	if (err)
		goto out;
	extent.fe_flags = FIEMAP_EXTENT_DELALLOC;
	err = expect("delalloc write extent", issue(dev_fd, &items[0]), -EOPNOTSUPP);
	if (err)
		goto out;
	extent.fe_flags = 0;

	/* Multiple block-device extents pass validation without issuing I/O. */
	extents[0].fe_logical = 0;
	extents[0].fe_physical = capacity - 4096;
	extents[0].fe_length = 512;
	extents[1].fe_logical = 512;
	extents[1].fe_physical = capacity - 2048;
	extents[1].fe_length = 512;
	items[0].ext_nr = 2;
	items[0].extents = (uint64_t)(uintptr_t)extents;
	iov.size = 1024;
	err = expect("block multi-extent write without topo", issue(dev_fd, &items[0]), -ENODEV);
	if (err)
		goto out;
	extents[1].fe_physical = capacity;
	err = expect("block second extent capacity", issue(dev_fd, &items[0]), -EFBIG);
	if (err)
		goto out;

	/* Regular-file writes accept contiguous multi-extent logical ranges
	 * once a topology exists; a logical gap is rejected before topo.
	 */
	items[0].file_fd = regular_fd;
	extents[0].fe_logical = 0;
	extents[0].fe_physical = 0;
	extents[0].fe_length = 512;
	extents[1].fe_logical = 1024;
	extents[1].fe_physical = 512;
	extents[1].fe_length = 512;
	err = expect("regular logical gap", issue(dev_fd, &items[0]), -EINVAL);
	if (err)
		goto out;

	extents[1].fe_logical = 512;
	err = expect("regular multi-extent write without topo", issue(dev_fd, &items[0]), -ENODEV);
	if (err)
		goto out;
	extents[1].fe_physical = 1ULL << 62;
	err = expect("regular second extent capacity", issue(dev_fd, &items[0]), -EFBIG);
	if (err)
		goto out;
	err = expect_no_event(dev_fd, "rejection-left-no-event");
	if (err)
		goto out;

	items[0].file_fd = write_fd;
	items[0].ext_nr = 1;
	items[0].extents = (uint64_t)(uintptr_t)&extent;
	extent.fe_logical = 0;
	extent.fe_physical = capacity - 4096;
	extent.fe_length = 512;
	iov.size = 1024;
	err = expect("insufficient extent coverage", issue(dev_fd, &items[0]), -E2BIG);
	if (err)
		goto out;
	iov.size = 512;
	extent.fe_physical = capacity;
	err = expect("out-of-capacity range", issue(dev_fd, &items[0]), -EFBIG);
	if (err)
		goto out;
	extent.fe_physical = capacity - 4096;

	/* Batch side effects: an invalid first item accepts nothing and
	 * leaves no completion event. No raw transfer is issued.
	 */
	items[1] = items[0];
	items[1].op = P2P_IO_WRITE + 1;
	items[0].op = P2P_IO_WRITE + 2;
	batch.nr = 2;
	batch.items = (uint64_t)(uintptr_t)items;
	err = expect("batch invalid first", issue_batch(dev_fd, &batch), -EINVAL);
	if (err)
		goto out;
	err = expect_no_event(dev_fd, "batch-reject-left-no-event");
	if (err)
		goto out;

	items[0].op = P2P_IO_WRITE;
	userspace.op = P2P_IO_WRITE + 1;
	userspace.file_name = regular_path;
	userspace.iov = &iov;
	userspace.iov_nr = 1;
	err = expect("userspace unknown operation", rw_file(-1, &userspace), -EINVAL);
	if (err)
		goto out;
	userspace.op = P2P_IO_WRITE;
	userspace.host_pid = -1;
	err = expect("userspace negative host pid", rw_file(-1, &userspace), -EINVAL);
	if (err)
		goto out;
	userspace.host_pid = getpid();
	userspace.flags = P2P_IO_F_REGISTERED_MEM;
	userspace.mem_handle = 1;
	err = expect("userspace registered host pid", rw_file(-1, &userspace), -EINVAL);
	if (err)
		goto out;
	userspace.host_pid = 0;
	userspace.flags = 0;
	userspace.mem_handle = 0;
	/* Extent preparation (fallocate + FIEMAP) runs before the ioctl, so a
	 * regular-file write now fails at the invalid device fd with EBADF
	 * rather than the removed EOPNOTSUPP gate.
	 */
	err = expect("userspace regular file write", rw_file(-1, &userspace), -EBADF);

out:
	if (write_fd >= 0)
		close(write_fd);
	if (read_fd >= 0)
		close(read_fd);
	if (regular_fd >= 0)
		close(regular_fd);
	unlink(regular_path);
	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	free(items);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
