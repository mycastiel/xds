/* Host-only regression: discover topology with no target /dev nodes.
 * Linker wrappers supply sysfs and capture ioctls; no device I/O runs.
 */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "file_p2p_api.h"
#include "nds_api.h"
#include "p2p_common.h"

#define BOOK_FD 100
#define TOPO_FD 101
#define SYSFS_FD 102
#define FILE_FD 103
#define BLOCK_FD 104
#define PIPE_FD 105
#define FILE_FD2 106

static const char *size_text = "8388609\n";
static const char *read_text;
static int open_count;
static int add_count;
static int del_count;
static int add_errno;
static int del_errno;
static int add_fail_at;
static int del_fail_at;
static int deleting;
static dev_t expected_dev;
static dev_t expected_dev2;
static unsigned long long expected_size;
static unsigned long long expected_size2;

/* The partition has a different size from its parent namespace. */
static const char *sysfs_text(const char *path)
{
	if (!strcmp(path, "/sys/dev/block/259:0/device/subsysnqn"))
		return "nqn.test\n";
	if (!strcmp(path, "/sys/dev/block/259:0/size"))
		return size_text;
	if (!strcmp(path, "/sys/dev/block/259:1/partition"))
		return "1\n";
	if (!strcmp(path, "/sys/dev/block/259:1/../dev"))
		return "259:0\n";
	if (!strcmp(path, "/sys/dev/block/259:1/size"))
		return "2049\n";
	return NULL;
}

int __wrap_stat(const char *path, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	/* Device-name fixtures for the file_p2p compatibility wrappers. */
	if (!strcmp(path, "/legacy/block") ||
	    !strcmp(path, "/legacy/partition")) {
		st->st_mode = S_IFBLK;
		st->st_rdev = makedev(259, !strcmp(path, "/legacy/partition"));
		return 0;
	}
	if (!strcmp(path, "/legacy/file")) {
		st->st_mode = S_IFREG;
		return 0;
	}
	assert(!deleting);
	/* In particular, all /dev/<block-device> paths are absent. */
	if (sysfs_text(path)) {
		st->st_mode = S_IFREG;
		return 0;
	}
	errno = ENOENT;
	return -1;
}

int __wrap_fstat(int fd, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	if (fd == FILE_FD || fd == FILE_FD2) {
		st->st_mode = S_IFREG;
		st->st_dev = makedev(259, 0);
	} else if (fd == BLOCK_FD) {
		st->st_mode = S_IFBLK;
		st->st_dev = makedev(0, 5);
		st->st_rdev = makedev(259, 1);
	} else if (fd == PIPE_FD) {
		st->st_mode = S_IFIFO;
	} else {
		errno = EBADF;
		return -1;
	}
	return 0;
}

int __wrap_open(const char *path, int flags, ...)
{
	assert(!deleting);
	assert(!strcmp(path, "/dev/p2p_device"));
	assert(flags == (O_RDWR | O_CLOEXEC));
	assert(open_count < 2);
	return BOOK_FD + open_count++;
}

int __wrap_openat(int dir_fd, const char *path, int flags, ...)
{
	assert(!deleting);
	assert(dir_fd == AT_FDCWD);
	assert(flags == (O_RDONLY | O_CLOEXEC));
	assert(!strncmp(path, "/sys/dev/block/", 15));
	read_text = sysfs_text(path);
	if (!read_text) {
		errno = ENOENT;
		return -1;
	}
	return SYSFS_FD;
}

ssize_t __wrap_read(int fd, void *buf, size_t size)
{
	size_t len;

	assert(fd == SYSFS_FD);
	assert(read_text);
	len = strlen(read_text);
	assert(len < size);
	memcpy(buf, read_text, len);
	return len;
}

int __wrap_close(int fd)
{
	assert(fd == BOOK_FD || fd == TOPO_FD || fd == SYSFS_FD);
	return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	assert(fd == TOPO_FD);
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (request == IOCTL_ADD_TOPO) {
		const struct topo_user_cfg *cfg = arg;

		assert(!strcmp(cfg->name, "nvme"));
		assert(cfg->top_dev == expected_dev ||
		       cfg->top_dev == expected_dev2);
		assert(cfg->nr_devs == 1);
		assert(!cfg->extra[0] && !cfg->extra[1]);
		assert(cfg->bdevs[0].dev_id == cfg->top_dev);
		assert(!cfg->bdevs[0].reserved);
		assert(!cfg->bdevs[0].start_sector);
		assert(cfg->bdevs[0].size_sector ==
		       (cfg->top_dev == expected_dev ? expected_size
						     : expected_size2));
		add_count++;
		if (add_errno || (add_fail_at && add_count == add_fail_at)) {
			errno = add_errno ? add_errno : EIO;
			return -1;
		}
	} else {
		const struct topo_del_cfg *cfg = arg;

		assert(request == IOCTL_DEL_TOPO);
		assert(cfg->top_dev == expected_dev ||
		       cfg->top_dev == expected_dev2);
		assert(!cfg->reserved);
		del_count++;
		if (del_errno || (del_fail_at && del_count == del_fail_at)) {
			errno = del_errno ? del_errno : EIO;
			return -1;
		}
	}
	return 0;
}

int main(void)
{
	struct nds_init_param init = { 0 };
	int32_t fd = FILE_FD;
	int32_t two_fds[2] = { FILE_FD, FILE_FD2 };
	int32_t mixed_fds[2] = { FILE_FD, BLOCK_FD };
	int32_t triple_fds[3] = { FILE_FD, FILE_FD2, BLOCK_FD };
	struct nds_fs_desc fs = { .fs_fd = &fd, .fs_fd_cnt = 1 };
	struct nds_fs_desc fs2 = { .fs_fd = two_fds, .fs_fd_cnt = 2 };
	struct nds_fs_desc fs3 = { .fs_fd = mixed_fds, .fs_fd_cnt = 2 };
	struct nds_fs_desc fs4 = { .fs_fd = triple_fds, .fs_fd_cnt = 3 };
	int before;
	int add_before;
	int del_before;

	expected_dev = makedev(259, 0);
	expected_size = 8388609;
	assert(p2p_add_topo(TOPO_FD, expected_dev) == 0);
	assert(add_count == 1);

	expected_dev = makedev(259, 1);
	expected_size = 2049;
	assert(p2p_add_topo(TOPO_FD, expected_dev) == 0);
	assert(add_count == 2);

	/* Failures before submission must not send a topology ioctl. */
	before = add_count;
	assert(p2p_add_topo(TOPO_FD, makedev(8, 0)) == -EOPNOTSUPP);
	size_text = "0\n";
	assert(p2p_add_topo(TOPO_FD, makedev(259, 0)) == -EINVAL);
	size_text = NULL;
	assert(p2p_add_topo(TOPO_FD, makedev(259, 0)) == -ENOENT);
	assert(add_count == before);
	size_text = "8388609\n";

	expected_dev = makedev(259, 0);
	expected_size = 8388609;
	add_errno = EIO;
	assert(p2p_add_topo(TOPO_FD, expected_dev) == -EIO);
	add_errno = 0;
	assert(add_topo(TOPO_FD, "/legacy/block") == 0);
	before = add_count;
	assert(add_topo(TOPO_FD, "/legacy/file") == -EINVAL);
	assert(add_topo(TOPO_FD, "/dev/missing") == -ENOENT);
	assert(add_count == before);
	assert(del_topo(TOPO_FD, "/legacy/file") == -EINVAL);
	assert(del_topo(TOPO_FD, "/dev/missing") == -ENOENT);
	assert(del_count == 0);

	/* Deletion needs only the device ID, with no sysfs or /dev lookup. */
	deleting = 1;
	assert(p2p_del_topo(TOPO_FD, expected_dev) == 0);
	expected_dev = makedev(259, 1);
	assert(del_topo(TOPO_FD, "/legacy/partition") == 0);
	del_errno = ENOENT;
	assert(p2p_del_topo(TOPO_FD, expected_dev) == -ENOENT);
	assert(del_topo(TOPO_FD, "/legacy/partition") == -ENOENT);
	del_errno = 0;
	assert(del_count == 4);
	deleting = 0;
	expected_dev = makedev(259, 0);

	/* NDS uses st_dev for a regular file and st_rdev for a block fd.
	 * Registration and deletion must identify the same topology.
	 */
	assert(nds_init(&init) == 0);
	assert(nds_register_fs(&fs) == 0);
	deleting = 1;
	assert(nds_unregister_fs(&fs) == 0);
	deleting = 0;
	fd = BLOCK_FD;
	expected_dev = makedev(259, 1);
	expected_size = 2049;
	assert(nds_register_fs(&fs) == 0);
	deleting = 1;
	assert(nds_unregister_fs(&fs) == 0);
	assert(del_count == 6);
	deleting = 0;
	before = add_count;
	fd = PIPE_FD;
	assert(nds_register_fs(&fs) == -EINVAL);
	assert(nds_unregister_fs(&fs) == -EINVAL);
	fd = -1;
	assert(nds_register_fs(&fs) == -EBADF);
	assert(nds_unregister_fs(&fs) == -EBADF);
	assert(add_count == before);
	assert(del_count == 6);

	/* Half-failure: register undoes the prefix. */
	expected_dev = makedev(259, 0);
	expected_size = 8388609;
	deleting = 0;
	add_before = add_count;
	del_before = del_count;
	add_fail_at = add_count + 2;
	assert(nds_register_fs(&fs2) == -EIO);
	assert(add_count == add_before + 2);
	assert(del_count == del_before + 1);
	add_fail_at = 0;

	/* Half-failure: unregister re-adds the prefix. */
	assert(nds_register_fs(&fs2) == 0);
	add_before = add_count;
	del_before = del_count;
	del_fail_at = del_count + 2;
	assert(nds_unregister_fs(&fs2) == -EIO);
	assert(del_count == del_before + 2);
	assert(add_count == add_before + 1);
	del_fail_at = 0;
	assert(nds_unregister_fs(&fs2) == 0);

	/* Distinct topologies: register undoes the registered prefix. */
	expected_dev = makedev(259, 0);
	expected_dev2 = makedev(259, 1);
	expected_size = 8388609;
	expected_size2 = 2049;
	add_before = add_count;
	del_before = del_count;
	add_fail_at = add_count + 2;
	assert(nds_register_fs(&fs3) == -EIO);
	assert(add_count == add_before + 2);
	assert(del_count == del_before + 1);
	add_fail_at = 0;

	/* Distinct topologies: unregister re-adds the removed prefix. */
	assert(nds_register_fs(&fs3) == 0);
	add_before = add_count;
	del_before = del_count;
	del_fail_at = del_count + 2;
	assert(nds_unregister_fs(&fs3) == -EIO);
	assert(del_count == del_before + 2);
	assert(add_count == add_before + 1);
	del_fail_at = 0;
	assert(nds_unregister_fs(&fs3) == 0);

	/* A rollback whose own ioctl fails still terminates: the nested
	 * register rollback runs and the original error is returned. */
	add_before = add_count;
	del_before = del_count;
	add_fail_at = add_count + 3;
	del_fail_at = del_count + 2;
	assert(nds_register_fs(&fs4) == -EIO);
	assert(add_count == add_before + 4);
	assert(del_count == del_before + 2);
	add_fail_at = 0;
	del_fail_at = 0;
	assert(nds_unregister_fs(&fs2) == 0);

	assert(nds_exit() == 0);
	puts("topo_devt_test: PASS");
	return 0;
}
