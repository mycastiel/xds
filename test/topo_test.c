#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_p2p_api.h"

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

int main(int argc, char **argv)
{
	const char *topology;
	struct stat st;
	int owner_fd = -1;
	int other_fd = -1;
	int err;

	if (argc != 3 || strcmp(argv[1], "--topology")) {
		fprintf(stderr, "Usage: %s --topology <block-device>\n", argv[0]);
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
