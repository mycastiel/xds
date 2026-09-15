#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "debugfs_test.h"
#include "file_p2p_api.h"

#define TOPO_DEBUGFS_PATH "/sys/kernel/debug/p2p_device/topo"
#define DEBUGFS_RACE_ITERATIONS 128

static int block_geometry(const char *path, dev_t *dev, unsigned long long *sectors)
{
	unsigned long long bytes;
	struct stat st;
	int fd;
	int err = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		err = -errno;
		goto out;
	}
	if (!S_ISBLK(st.st_mode)) {
		err = -EINVAL;
		goto out;
	}
	if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) {
		err = -errno;
		goto out;
	}
	*dev = st.st_rdev;
	*sectors = bytes / 512;
out:
	close(fd);
	return err;
}

static int expect(const char *name, int actual, int expected)
{
	if (actual == expected) {
		printf("api=c case=%s ret=%d expected=%d\n",
		       name, actual, expected);
		return 0;
	}
	fprintf(stderr, "%s returned %d, expected %d\n",
		name, actual, expected);
	return -EINVAL;
}

static int del_topo_reserved(int dev_fd, dev_t top_dev)
{
	struct topo_del_cfg cfg = {
		.top_dev = top_dev,
		.reserved = 1,
	};

	if (ioctl(dev_fd, IOCTL_DEL_TOPO, &cfg) < 0)
		return -errno;
	return 0;
}

static int check_raid0_cfg(const char *name, struct topo_user_cfg *cfg, int expected)
{
	int fd = new_p2p_fd();
	int err;

	if (fd < 0)
		return fd;
	err = ioctl(fd, IOCTL_ADD_TOPO, cfg) < 0 ? -errno : 0;
	close_p2p_fd(fd);
	return expect(name, err, expected);
}

/* Register synthetic ranges only; never issue I/O against these mappings.
 * The prepared VM creates two-member arrays with 64 KiB chunks.
 */
static int test_raid0_offsets(const char *topology, const char *member1,
			      const char *member2)
{
	const unsigned long long chunk_sectors = 128;
	const char *members[] = { member1, member2 };
	struct topo_user_cfg *cfg;
	unsigned long long sectors = 0;
	unsigned long long capacity = 0;
	dev_t dev = 0;
	unsigned int i;
	int err;

	err = block_geometry(topology, &dev, &sectors);
	if (err)
		return err;
	if (!sectors || sectors % (2 * chunk_sectors))
		return -EINVAL;
	cfg = calloc(1, sizeof(*cfg) + 2 * sizeof(cfg->bdevs[0]));
	if (!cfg)
		return -ENOMEM;
	strcpy(cfg->name, "raid0");
	cfg->top_dev = dev;
	cfg->nr_devs = 2;
	cfg->extra[0] = 7;
	for (i = 0; i < cfg->nr_devs; i++) {
		err = block_geometry(members[i], &dev, &capacity);
		if (err)
			goto out;
		/* Leave room for both aligned starts and a one-sector perturbation. */
		if (capacity <= sectors / 2 + 2 * chunk_sectors) {
			err = -EINVAL;
			goto out;
		}
		cfg->bdevs[i].dev_id = dev;
		cfg->bdevs[i].size_sector = sectors / 2;
		cfg->bdevs[i].start_sector = chunk_sectors;
	}
	err = check_raid0_cfg("raid0 equal aligned starts", cfg, 0);
	if (err)
		goto out;
	cfg->bdevs[1].start_sector = 2 * chunk_sectors;
	err = check_raid0_cfg("raid0 different aligned starts", cfg, 0);
	if (err)
		goto out;
	for (i = 0; i < cfg->nr_devs; i++) {
		cfg->bdevs[i].start_sector++;
		err = check_raid0_cfg(i ? "raid0 unaligned second start" :
				     "raid0 unaligned first start", cfg, 0);
		cfg->bdevs[i].start_sector--;
		if (err)
			goto out;
	}
	cfg->bdevs[1].size_sector -= chunk_sectors;
	err = check_raid0_cfg("raid0 unequal member sizes", cfg, -EINVAL);
out:
	free(cfg);
	return err;
}

static int test_debugfs_topo_race(int dev_fd, const char *topology, dev_t top_dev)
{
	struct debugfs_reader_context reader;
	char expected[64];
	bool added = false;
	unsigned int i;
	int reader_err;
	int err;

	err = add_topo(dev_fd, topology);
	if (err)
		return err;
	added = true;
	snprintf(expected, sizeof(expected), "top_dev: %u:%u", major(top_dev), minor(top_dev));
	err = debugfs_read_file(TOPO_DEBUGFS_PATH, expected);
	if (err)
		goto out;
	err = del_topo(dev_fd, topology);
	if (err)
		goto out;
	added = false;
	err = debugfs_read_file(TOPO_DEBUGFS_PATH, expected);
	if (err != -ENOENT) {
		if (!err)
			err = -EEXIST;
		goto out;
	}
	err = 0;

	err = debugfs_reader_start(&reader, TOPO_DEBUGFS_PATH);
	if (err)
		return err;
	for (i = 0; i < DEBUGFS_RACE_ITERATIONS; i++) {
		err = add_topo(dev_fd, topology);
		if (err)
			break;
		added = true;
		sched_yield();
		err = del_topo(dev_fd, topology);
		if (err)
			break;
		added = false;
		sched_yield();
	}
	reader_err = debugfs_reader_stop(&reader);
	if (!err)
		err = reader_err;

out:
	if (added)
		del_topo(dev_fd, topology);
	return err;
}

int main(int argc, char **argv)
{
	const char *topology;
	struct stat st;
	int owner_fd = -1;
	int other_fd = -1;
	int err;

	if (argc == 6 && !strcmp(argv[1], "--topology") &&
	    !strcmp(argv[3], "--raid0-members")) {
		err = test_raid0_offsets(argv[2], argv[4], argv[5]);
		if (err)
			fprintf(stderr, "RAID0 offset test failed: %s (%d)\n",
				strerror(-err), err);
		return err ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc != 3 || strcmp(argv[1], "--topology")) {
		fprintf(stderr, "Usage: %s --topology <block-device> "
			"[--raid0-members <member1> <member2>]\n", argv[0]);
		return EXIT_FAILURE;
	}
	topology = argv[2];
	if (stat(topology, &st) < 0 || !S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%s is not a block device\n", topology);
		return EXIT_FAILURE;
	}

	owner_fd = new_p2p_fd();
	if (owner_fd < 0) {
		err = owner_fd;
		goto out;
	}
	other_fd = new_p2p_fd();
	if (other_fd < 0) {
		err = other_fd;
		goto out;
	}

	err = expect("del topology reserved",
		     del_topo_reserved(owner_fd, st.st_rdev), -EINVAL);
	if (err)
		goto out;
	err = expect("del topology missing", del_topo(owner_fd, topology),
		     -ENOENT);
	if (err)
		goto out;
	err = expect("add topology owner", add_topo(owner_fd, topology), 0);
	if (err)
		goto out;
	err = expect("del topology foreign fd", del_topo(other_fd, topology),
		     -ENOENT);
	if (err)
		goto out;
	err = expect("add topology other", add_topo(other_fd, topology), 0);
	if (err)
		goto out;
	err = expect("del topology owner", del_topo(owner_fd, topology), 0);
	if (err)
		goto out;
	err = expect("del topology owner stale", del_topo(owner_fd, topology),
		     -ENOENT);
	if (err)
		goto out;
	err = expect("del topology other", del_topo(other_fd, topology), 0);
	if (err)
		goto out;
	err = expect("del topology other stale", del_topo(other_fd, topology),
		     -ENOENT);
	if (err)
		goto out;

	err = expect("add topology duplicate first", add_topo(owner_fd, topology),
		     0);
	if (err)
		goto out;
	err = expect("add topology duplicate second", add_topo(owner_fd, topology),
		     0);
	if (err)
		goto out;
	err = expect("del topology duplicate first", del_topo(owner_fd, topology),
		     0);
	if (err)
		goto out;
	err = expect("del topology duplicate second", del_topo(owner_fd, topology),
		     0);
	if (err)
		goto out;
	err = expect("del topology duplicate stale", del_topo(owner_fd, topology),
		     -ENOENT);
	if (err)
		goto out;
	err = expect("debugfs topology add/delete race",
		     test_debugfs_topo_race(owner_fd, topology, st.st_rdev), 0);

out:
	if (other_fd >= 0)
		close_p2p_fd(other_fd);
	if (owner_fd >= 0)
		close_p2p_fd(owner_fd);
	if (err) {
		fprintf(stderr, "topo_test failed: %s (%d)\n",
			strerror(-err), err);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
