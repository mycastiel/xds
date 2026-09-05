#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nds_api.h"

int main(int argc, char **argv)
{
	struct nds_init_param init = { 0 };
	struct nds_fs_desc desc = { 0 };
	int32_t topology_fd;
	int fd = -1;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <topology-device>\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDONLY | O_DIRECT);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", argv[1],
			strerror(errno));
		return EXIT_FAILURE;
	}

	ret = nds_init(&init);
	if (ret) {
		fprintf(stderr, "nds_init failed: %s (%d)\n", strerror(-ret), ret);
		goto out;
	}
	if (init.version != NDS_API_VERSION) {
		fprintf(stderr, "unexpected NDS API version: %u\n", init.version);
		ret = -EPROTO;
		goto out_nds;
	}

	topology_fd = fd;
	desc.fs_fd = &topology_fd;
	desc.fs_fd_cnt = 1;
	ret = nds_register_fs(&desc);
	if (ret) {
		fprintf(stderr, "nds_register_fs failed: %s (%d)\n",
			strerror(-ret), ret);
		goto out_nds;
	}

	ret = nds_unregister_fs(&desc);
	if (ret)
		fprintf(stderr, "nds_unregister_fs failed: %s (%d)\n",
			strerror(-ret), ret);
	else
		printf("NDS topology registration passed for %s\n", argv[1]);

out_nds:
	nds_exit();
out:
	close(fd);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
