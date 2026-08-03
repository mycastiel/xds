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

_Static_assert(sizeof(struct p2p_iov) == 16,
	       "p2p_iov must remain a 16-byte UAPI record");
_Static_assert(offsetof(struct p2p_iov, reserved) == 12,
	       "p2p_iov reserved field must occupy the old padding");
_Static_assert(sizeof(((struct p2p_io_param *)0)->iov) == sizeof(uint64_t),
	       "p2p_io_param iov address must be fixed-width");
_Static_assert(sizeof(((struct p2p_getevents_param *)0)->events) ==
	       sizeof(uint64_t),
	       "p2p_getevents_param events address must be fixed-width");

static int expect(const char *name, int actual, int expected)
{
	if (actual == expected) {
		printf("api=c case=%s ret=%d expected=%d\n",
		       name, actual, expected);
		return 0;
	}
	fprintf(stderr, "%s returned %d, expected %d\n", name, actual,
		expected);
	return -EINVAL;
}

static int issue(int dev_fd, struct p2p_io_param *param)
{
	if (ioctl(dev_fd, IOCTL_RW_FILE, param) < 0)
		return -errno;
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "target", required_argument, NULL, 't' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct p2p_io_param *param;
	struct io_parameter userspace = { };
	struct p2p_iov iov = {
		.addr = 0,
		.size = 512,
	};
	const char *target = NULL;
	char regular_path[] = "/tmp/xds-rw-uapi.XXXXXX";
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

	param = calloc(1, sizeof(*param) + sizeof(param->extents[0]));
	if (!param)
		return EXIT_FAILURE;
	param->op = P2P_IO_WRITE;
	param->host_pid = getpid();
	param->iov = (uint64_t)(uintptr_t)&iov;
	param->iov_nr = 1;
	param->ext_nr = 1;
	param->extents[0].fe_logical = 0;
	param->extents[0].fe_physical = 0;
	param->extents[0].fe_length = 512;

	dev_fd = new_p2p_fd();
	if (dev_fd < 0) {
		err = dev_fd;
		goto out;
	}
	regular_fd = mkstemp(regular_path);
	if (regular_fd < 0) {
		err = -errno;
		goto out;
	}
	if (ftruncate(regular_fd, 4096)) {
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

	param->op = P2P_IO_WRITE + 1;
	err = expect("unknown operation", issue(dev_fd, param), -EINVAL);
	if (err)
		goto out;
	param->op = P2P_IO_WRITE;
	param->flags = P2P_IO_F_MASK << 1;
	err = expect("unknown flags", issue(dev_fd, param), -EOPNOTSUPP);
	if (err)
		goto out;
	param->flags = 0;
	iov.reserved = 1;
	err = expect("IOV reserved field", issue(dev_fd, param), -EINVAL);
	if (err)
		goto out;
	iov.reserved = 0;
	iov.size = TEST_TOO_LARGE_IOV_SIZE;
	err = expect("IOV exceeds 2 GiB", issue(dev_fd, param), -EINVAL);
	if (err)
		goto out;
	iov.size = 512;

	param->file_fd = regular_fd;
	err = expect("regular file write", issue(dev_fd, param),
		     -EOPNOTSUPP);
	if (err)
		goto out;
	param->file_fd = read_fd;
	err = expect("read-only block fd", issue(dev_fd, param), -EBADF);
	if (err)
		goto out;

	param->file_fd = write_fd;
	iov.size = 1024;
	err = expect("insufficient extent coverage", issue(dev_fd, param),
		     -E2BIG);
	if (err)
		goto out;
	iov.size = 512;
	param->extents[0].fe_logical = capacity;
	param->extents[0].fe_physical = capacity;
	err = expect("out-of-capacity range", issue(dev_fd, param), -EFBIG);
	if (err)
		goto out;

	userspace.op = P2P_IO_WRITE + 1;
	userspace.file_name = regular_path;
	userspace.iov = &iov;
	userspace.iov_nr = 1;
	err = expect("userspace unknown operation", rw_file(-1, &userspace),
		     -EINVAL);
	if (err)
		goto out;
	userspace.op = P2P_IO_WRITE;
	userspace.host_pid = -1;
	err = expect("userspace negative host pid",
		     rw_file(-1, &userspace), -EINVAL);
	if (err)
		goto out;
	userspace.host_pid = getpid();
	userspace.flags = P2P_IO_F_REGISTERED_MEM;
	userspace.mem_handle = 1;
	err = expect("userspace registered host pid",
		     rw_file(-1, &userspace), -EINVAL);
	if (err)
		goto out;
	userspace.host_pid = 0;
	userspace.flags = 0;
	userspace.mem_handle = 0;
	err = expect("userspace regular file write", rw_file(-1, &userspace),
		     -EOPNOTSUPP);

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
	free(param);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
