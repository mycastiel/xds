/*
 * nds_example.c — end-to-end NDS usage from init to I/O completion.
 *
 * Demonstrates the recommended hot path:
 *   nds_init → nds_register_mem → nds_io_new_ctx →
 *   nds_io_submit → nds_io_getevents → teardown
 *
 * Build (from the repository root):
 *   make -C test nds_example
 *
 * Run (requires /dev/p2p_device and a loaded topology-capable block stack):
 *   sudo ./test/nds_example /dev/nvme0n1 /path/to/file.dat 0 0 4096
 *
 * Args:
 *   topology   file or block device identifying the I/O topology
 *   file       target file / block device to read
 *   hbm_va     HBM (or CMB) virtual address of the I/O buffer (default 0)
 *   offset     byte offset in @file (default 0, must be 512-aligned)
 *   length     transfer length in bytes (default 4096, must be 512-aligned)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nds_api.h"

static void die(const char *what, int err)
{
	fprintf(stderr, "%s: %s (%d)\n", what,
		err < 0 ? strerror(-err) : strerror(errno),
		err < 0 ? err : -errno);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	const char *topo_path;
	const char *file_path;
	uint64_t hbm_va = 0;
	uint64_t offset = 0;
	uint32_t length = 4096;
	int topo_fd = -1;
	int file_fd = -1;
	int32_t fs_fds[1];
	struct nds_init_param init_param = {
		.desc = { .fs_fd = fs_fds, .fs_fd_cnt = 1 },
	};
	struct nds_io_ctx *ctx = NULL;
	struct nds_io_cb cb;
	struct nds_io_vec iov;
	struct nds_io_event ev;
	int err;
	int n;

	if (argc < 3 || argc > 6) {
		fprintf(stderr,
			"usage: %s <topology> <file> [hbm_va] [offset] [length]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	topo_path = argv[1];
	file_path = argv[2];
	if (argc > 3)
		hbm_va = strtoull(argv[3], NULL, 0);
	if (argc > 4)
		offset = strtoull(argv[4], NULL, 0);
	if (argc > 5)
		length = (uint32_t)strtoul(argv[5], NULL, 0);

	/* ---- 1. Open topology + target FDs ---- */
	topo_fd = open(topo_path, O_RDONLY | O_DIRECT);
	if (topo_fd < 0)
		die("open topology", -errno);

	file_fd = open(file_path, O_RDONLY | O_DIRECT);
	if (file_fd < 0)
		die("open file", -errno);

	/* ---- 2. Process-wide init (registers topology once) ---- */
	fs_fds[0] = topo_fd;
	err = nds_init(&init_param);
	if (err)
		die("nds_init", err);
	printf("XDS API version: %u\n", init_param.version);

	/*
	 * ---- 3. Register the HBM window (optional but recommended) ----
	 * After this, submits use NDS_IO_F_REGISTERED_MEM; the library looks
	 * up the kernel mem_handle from buf_addr — callers never see it.
	 */
	err = nds_register_mem((void *)(uintptr_t)hbm_va, length, 0);
	if (err)
		die("nds_register_mem", err);

	/* ---- 4. Create an I/O context (queue) ---- */
	err = nds_io_new_ctx(
		&(struct nds_io_ctx_param){ .max_io_cnt = 32 }, &ctx);
	if (err)
		die("nds_io_new_ctx", err);

	/* ---- 5. Build one async PREAD and submit ---- */
	memset(&cb, 0, sizeof(cb));
	memset(&iov, 0, sizeof(iov));
	iov.buf_addr = hbm_va;
	iov.buf_len = length;
	cb.opcode = NDS_IO_OP_PREAD;
	cb.rw_flags = NDS_IO_F_REGISTERED_MEM;
	cb.obj.fd = file_fd;
	cb.offset = offset;
	cb.iov = &iov;
	cb.iov_cnt = 1;
	cb.host_pid = 0;		/* must be 0 with REGISTERED_MEM */
	cb.user_data = 0xA5A5;		/* echoed in the completion event */

	n = nds_io_submit(ctx, 1, &cb);
	if (n != 1)
		die("nds_io_submit", n < 0 ? n : -EIO);

	/* ---- 6. Wait for completion ---- */
	memset(&ev, 0, sizeof(ev));
	n = nds_io_getevents(ctx, 1, 1, &ev, NULL); /* NULL = wait forever */
	if (n != 1)
		die("nds_io_getevents", n < 0 ? n : -EIO);
	if (ev.res < 0)
		die("I/O failed", (int)ev.res);

	printf("I/O complete: user_data=0x%llx res=%lld\n",
	       (unsigned long long)ev.user_data, (long long)ev.res);

	/* ---- 7. Tear down (reverse order) ---- */
	err = nds_io_destroy_ctx(ctx);
	if (err)
		die("nds_io_destroy_ctx", err);
	ctx = NULL;

	err = nds_unregister_mem((void *)(uintptr_t)hbm_va);
	if (err)
		die("nds_unregister_mem", err);

	err = nds_exit();
	if (err)
		die("nds_exit", err);

	close(file_fd);
	close(topo_fd);
	return EXIT_SUCCESS;
}
