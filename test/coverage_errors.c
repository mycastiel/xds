/* Short raw-UAPI error tests. Run only with the CMB stub on a disposable VM. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>
#include "../p2p_dev_uapi.h"

#define PTR(p) ((uint64_t)(uintptr_t)(p))
static int failures;
static int call(int fd, unsigned long op, void *arg)
{
	int n = ioctl(fd, op, arg);
	return n < 0 ? -errno : n;
}
static void check(const char *name, int got, int want)
{
	printf("%s api=c case=%s ret=%d expected=%d\n",
	       got == want ? "PASS" : "FAIL", name, got, want);
	failures += got != want;
}
static void check_either(const char *name, int got, int want, int alternate)
{
	int ok = got == want || got == alternate;
	printf("%s api=c case=%s ret=%d expected=%d|%d\n",
	       ok ? "PASS" : "FAIL", name, got, want, alternate);
	failures += !ok;
}
static void mark(int enabled)
{
	if (getenv("XDS_INJECT")) {
		int fd = open("/proc/self/make-it-fail", O_WRONLY);
		if (fd < 0 || write(fd, enabled ? "1" : "0", 1) != 1) exit(2);
		close(fd);
	}
}
static int read_one(int fd, struct p2p_io_param *io)
{
	struct p2p_io_batch_param batch = { .items = PTR(io), .nr = 1 };
	struct p2p_io_event event = {0};
	struct p2p_getevents_param events = {
		.min_nr = 1, .max_nr = 1, .timeout_ns = 2000000000,
		.events = PTR(&event),
	};
	int ret = call(fd, IOCTL_SUBMIT_IO, &batch);
	if (ret != 1) return ret < 0 ? ret : -EPROTO;
	ret = call(fd, IOCTL_GET_IO_EVENTS, &events);
	return ret == 1 ? event.res : (ret < 0 ? ret : -ETIMEDOUT);
}
int main(int argc, char **argv)
{
	struct { struct topo_user_cfg cfg; struct topo_user_bdev bdev; } topo = {0};
	struct p2p_mem_register_param mem = { .size = 4096 };
	struct p2p_iov iov = { .size = 512 };
	struct fiemap_extent ext = { .fe_length = 512 };
	struct p2p_io_param io = { .host_pid = getpid(), .iov = PTR(&iov),
		.iov_nr = 1, .ext_nr = 1, .extents = PTR(&ext) };
	struct stat st;
	uint64_t bytes;
	int fd, target, ret;
	if (argc != 3) { fprintf(stderr, "usage: %s /dev/nvmeNn1 easy|open|register|topo|io|rio|range\n", argv[0]); return 2; }
	target = open(argv[1], O_RDONLY);
	if (target < 0 || fstat(target, &st) || !S_ISBLK(st.st_mode) ||
	    ioctl(target, BLKGETSIZE64, &bytes)) return 2;
	io.file_fd = target;
	strcpy(topo.cfg.name, "nvme");
	topo.cfg.top_dev = topo.bdev.dev_id = st.st_rdev;
	topo.cfg.nr_devs = 1;
	topo.bdev.size_sector = bytes / 512;
	if (!strcmp(argv[2], "open")) mark(1);
	fd = open("/dev/p2p_device", O_RDWR);
	if (!strcmp(argv[2], "open")) { ret = fd < 0 ? -errno : 0; mark(0); goto result; }
	if (fd < 0) return 2;
	if (!strcmp(argv[2], "easy")) {
		check("unknown ioctl", call(fd, _IO('k', 99), NULL), -EINVAL);
		io.iov = 1;
		check("unreadable iov", read_one(fd, &io), -EFAULT);
		io.iov = PTR(&iov); io.extents = 1;
		check("unreadable extents", read_one(fd, &io), -EFAULT);
		io.extents = PTR(&ext); io.file_fd = -1;
		check("closed target fd", read_one(fd, &io), -EBADF);
		io.file_fd = target;
		check("missing topology", read_one(fd, &io), -ENODEV);
		int tmp = memfd_create("xds-coverage", 0);
		if (tmp < 0) return 2;
		io.file_fd = tmp;
		check("RAM file has no block device", read_one(fd, &io), -EOPNOTSUPP);
		close(tmp); io.file_fd = target;
		long page = sysconf(_SC_PAGESIZE);
		char *map = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (map == MAP_FAILED) return 2;
		memcpy(map + page - sizeof(topo.cfg), &topo.cfg, sizeof(topo.cfg));
		if (mprotect(map + page, page, PROT_NONE)) return 2;
		check("topology header only", call(fd, IOCTL_ADD_TOPO, map + page - sizeof(topo.cfg)), -EFAULT);
		memcpy(map, &mem, sizeof(mem));
		if (mprotect(map, page, PROT_READ)) return 2;
		check("register copyout rollback", call(fd, IOCTL_REGISTER_MEM, map), -EFAULT);
		munmap(map, page * 2);
		topo.cfg.top_dev = 0;
		check("missing top device", call(fd, IOCTL_ADD_TOPO, &topo), -ENXIO);
		topo.cfg.top_dev = st.st_rdev;
		strcpy(topo.cfg.name, "linear");
		topo.bdev.dev_id = 0;
		check("wrong topology backend", call(fd, IOCTL_ADD_TOPO, &topo), -EOPNOTSUPP);
		strcpy(topo.cfg.name, "nvme");
		topo.bdev.dev_id = 0;
		check("missing component", call(fd, IOCTL_ADD_TOPO, &topo), -ENXIO);
		topo.bdev.dev_id = st.st_rdev;
	}
	if (!strcmp(argv[2], "register")) {
		mark(1); ret = call(fd, IOCTL_REGISTER_MEM, &mem); mark(0);
		if (!ret) {
			struct p2p_mem_unregister_param unreg = { .mem_handle = mem.mem_handle };
			check("unregister", call(fd, IOCTL_UNREGISTER_MEM, &unreg), 0);
		}
		goto result;
	}
	if (!strcmp(argv[2], "range")) {
		if (!getenv("XDS_COMPONENT") || stat(getenv("XDS_COMPONENT"), &st)) return 2;
		strcpy(topo.cfg.name, "linear");
		topo.bdev.dev_id = st.st_rdev;
	}
	if (!strcmp(argv[2], "topo")) mark(1);
	ret = call(fd, IOCTL_ADD_TOPO, &topo);
	if (!strcmp(argv[2], "topo")) { mark(0); goto result; }
	if (ret) { check("topology setup", ret, 0); goto result; }
	if (!strcmp(argv[2], "rio")) {
		ret = call(fd, IOCTL_REGISTER_MEM, &mem);
		if (ret) return 2;
		io.host_pid = 0; io.flags = P2P_IO_F_REGISTERED_MEM;
		io.mem_handle = mem.mem_handle;
	}
	if (!strcmp(argv[2], "io") || !strcmp(argv[2], "rio")) mark(1);
	ret = read_one(fd, &io);
	mark(0);
	if (!strcmp(argv[2], "rio")) {
		struct p2p_mem_unregister_param unreg = { .mem_handle = mem.mem_handle };
		check("unregister after I/O", call(fd, IOCTL_UNREGISTER_MEM, &unreg), 0);
	}
	if (!strcmp(argv[2], "easy")) {
		check("recovery read", ret, 0);
		ext.fe_physical = bytes;
		check_either("read outside namespace", read_one(fd, &io), -EIO, -EREMOTEIO);
		ext.fe_physical = 0;
		check("recovery after issue error", read_one(fd, &io), 0);
	}
	if (!strcmp(argv[2], "range")) {
		check("linear read", ret, 0);
		ext.fe_physical = bytes;
		check("linear mapping rejection", read_one(fd, &io), -EINVAL);
		ext.fe_physical = 0;
		check("linear recovery", read_one(fd, &io), 0);
		struct p2p_io_batch_param batch = { .items = PTR(&io), .nr = 1 };
		ext.fe_physical = bytes;
		check("submit for drain", call(fd, IOCTL_SUBMIT_IO, &batch), 1);
		check("drain reports issue error", call(fd, IOCTL_DRAIN_IO, NULL), -EINVAL);
		check("leave error event for close", call(fd, IOCTL_SUBMIT_IO, &batch), 1);
	}
result:
	printf("RESULT %d\n", ret);
	if (fd >= 0) close(fd);
	close(target);
	return failures || (ret != 0 && ret != -ENOMEM && ret != -EIO &&
		!(getenv("XDS_INJECT") && ret == -EOPNOTSUPP));
}
