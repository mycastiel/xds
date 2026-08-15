/*
 * nds_crc_verify — physical Ascend NDS PREAD correctness check.
 *
 * Reads [offset, offset+length) from a file into HBM via NDS, copies HBM
 * back to host with aclrtMemcpy, and compares CRC32 of the HBM payload
 * against CRC32 of the same range read with O_DIRECT pread.
 *
 * Kernel P2P_MIN_PAGE_SIZE is 64KiB. Physical Ascend needs a P2P-huge HBM
 * window (default ≥2MiB, acl policy HUGE_FIRST_P2P) so get_mem_page_size is
 * not 4K. I/O may use a shorter prefix (e.g. 4K) inside that window with
 * register_mem. --no-register-mem requires --length >= 2M (one-shot pins iov).
 *
 * CRC32 contract matches crc32_verify.c / kernel crc32_le:
 *   poly 0xedb88320, init ~0, final XOR ~0.
 *
 * Build: make -C test nds_crc_verify
 * Example:
 *   source /usr/local/Ascend/ascend-toolkit/set_env.sh
 *   sudo -E ./test/nds_crc_verify --topology /dev/nvme0n1 \
 *        --target /mnt/data/npu0.bin --npu-device 0 \
 *        --offset 0 --length 1M
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nds_api.h"
#include "hbm_acl.h"

#define SECTOR 512u
/* Practical min window so device huge / P2P pages are used. */
#define P2P_HBM_ALLOC_MIN (2u << 20)
#define CRC32_POLY_LE 0xedb88320U

static size_t hbm_alloc_size(uint32_t io_len)
{
	size_t n = io_len < P2P_HBM_ALLOC_MIN ? P2P_HBM_ALLOC_MIN :
						 (size_t)io_len;

	return (n + P2P_HBM_ALLOC_MIN - 1) & ~(size_t)(P2P_HBM_ALLOC_MIN - 1);
}

static void die(const char *what, int err)
{
	fprintf(stderr, "%s: %s (%d)\n", what,
		err < 0 ? strerror(-err) : strerror(errno),
		err < 0 ? -err : errno);
	exit(EXIT_FAILURE);
}

static uint64_t parse_size(const char *s)
{
	char *end = NULL;
	unsigned long long v = strtoull(s, &end, 0);
	unsigned long long mul = 1;

	if (!end || end == s)
		die("parse size", -EINVAL);
	switch (*end) {
	case 'G':
	case 'g':
		mul = 1ull << 30;
		end++;
		break;
	case 'M':
	case 'm':
		mul = 1ull << 20;
		end++;
		break;
	case 'K':
	case 'k':
		mul = 1ull << 10;
		end++;
		break;
	case '\0':
		break;
	default:
		die("parse size suffix", -EINVAL);
	}
	if (*end)
		die("parse size trailing junk", -EINVAL);
	if (v > UINT64_MAX / mul)
		die("parse size overflow", -EOVERFLOW);
	return (uint64_t)(v * mul);
}

static uint64_t target_size_bytes(int fd)
{
	struct stat st;
	uint64_t bytes;

	if (fstat(fd, &st) < 0)
		die("fstat target", -errno);
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &bytes) < 0)
			die("BLKGETSIZE64", -errno);
		return bytes;
	}
	if (S_ISREG(st.st_mode))
		return (uint64_t)st.st_size;
	die("target must be block or regular file", -EINVAL);
	return 0;
}

static void init_crc32_table(uint32_t table[256])
{
	unsigned int i;

	for (i = 0; i < 256; i++) {
		uint32_t crc = i;
		unsigned int bit;

		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? CRC32_POLY_LE : 0);
		table[i] = crc;
	}
}

static uint32_t crc32_buf(const unsigned char *data, size_t length)
{
	uint32_t table[256];
	uint32_t crc = ~0U;

	init_crc32_table(table);
	while (length--) {
		crc = (crc >> 8) ^ table[(crc ^ *data) & 0xff];
		data++;
	}
	return crc ^ ~0U;
}

static void *aligned_alloc_sector(size_t size)
{
	void *p = NULL;
	int err;

	err = posix_memalign(&p, SECTOR, size);
	if (err || !p)
		die("posix_memalign", err ? -err : -ENOMEM);
	memset(p, 0, size);
	return p;
}

static void read_file_range(int fd, uint64_t offset, uint32_t length,
			    unsigned char *buf)
{
	uint32_t done = 0;

	while (done < length) {
		ssize_t n = pread(fd, buf + done, length - done,
				  (off_t)(offset + done));

		if (n < 0)
			die("pread target", -errno);
		if (n == 0)
			die("pread short/EOF", -EIO);
		done += (uint32_t)n;
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s --topology DEV --target PATH --npu-device ID "
		"--offset BYTES --length BYTES [options]\n"
		"  --acl-policy N      aclrt malloc policy "
		"(default 3=HUGE_FIRST_P2P)\n"
		"  --no-register-mem   one-shot VAs (default: register);\n"
		"                      requires --length >= 2M\n",
		argv0);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "target", required_argument, NULL, 'f' },
		{ "npu-device", required_argument, NULL, 'd' },
		{ "offset", required_argument, NULL, 'o' },
		{ "length", required_argument, NULL, 'l' },
		{ "acl-policy", required_argument, NULL, 'A' },
		{ "no-register-mem", no_argument, NULL, 'N' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topo_path = NULL;
	const char *target_path = NULL;
	int npu_device = -1;
	int acl_policy = XDS_ACL_MEM_MALLOC_HUGE_FIRST_P2P;
	uint64_t offset = 0;
	uint64_t length_u64 = 0;
	uint32_t length = 0;
	bool register_mem = true;
	bool have_offset = false;
	bool have_length = false;
	struct xds_hbm_buf hbm = { 0 };
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
	unsigned char *file_buf = NULL;
	unsigned char *hbm_buf = NULL;
	uint64_t target_bytes;
	uint32_t file_crc;
	uint32_t hbm_crc;
	size_t alloc_size;
	int err;
	int n;
	int ok;

	for (;;) {
		int c = getopt_long(argc, argv, "t:f:d:o:l:A:Nh", opts, NULL);

		if (c == -1)
			break;
		switch (c) {
		case 't':
			topo_path = optarg;
			break;
		case 'f':
			target_path = optarg;
			break;
		case 'd':
			npu_device = (int)strtol(optarg, NULL, 0);
			break;
		case 'o':
			offset = parse_size(optarg);
			have_offset = true;
			break;
		case 'l':
			length_u64 = parse_size(optarg);
			have_length = true;
			break;
		case 'A':
			acl_policy = (int)strtol(optarg, NULL, 0);
			break;
		case 'N':
			register_mem = false;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!topo_path || !target_path || npu_device < 0 || !have_offset ||
	    !have_length || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!length_u64 || length_u64 > UINT32_MAX)
		die("length out of range", -EINVAL);
	length = (uint32_t)length_u64;
	if ((offset | length) & (SECTOR - 1))
		die("offset/length must be 512B-aligned", -EINVAL);
	/*
	 * One-shot pin asks get_mem_page_size(iov). Require ≥2MiB so Ascend
	 * reports a device huge page, not 4K.
	 */
	if (!register_mem && length < P2P_HBM_ALLOC_MIN) {
		fprintf(stderr,
			"nds_crc_verify: --no-register-mem requires "
			"--length >= %u (got %u); use register_mem for "
			"smaller I/Os\n",
			P2P_HBM_ALLOC_MIN, length);
		return EXIT_FAILURE;
	}

	alloc_size = hbm_alloc_size(length);

	topo_fd = open(topo_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (topo_fd < 0)
		die("open topology", -errno);
	file_fd = open(target_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (file_fd < 0)
		die("open target", -errno);

	target_bytes = target_size_bytes(file_fd);
	if (offset > target_bytes || length > target_bytes - offset)
		die("range exceeds target", -EINVAL);

	file_buf = aligned_alloc_sector(length);
	hbm_buf = aligned_alloc_sector(length);
	read_file_range(file_fd, offset, length, file_buf);
	file_crc = crc32_buf(file_buf, length);

	fprintf(stderr,
		"nds_crc_verify: alloc_size=%zu acl_policy=%d register_mem=%d "
		"io_length=%u\n",
		alloc_size, acl_policy, register_mem ? 1 : 0, length);

	err = xds_hbm_alloc(&hbm, npu_device, alloc_size, acl_policy);
	if (err)
		die("xds_hbm_alloc", err);

	fs_fds[0] = topo_fd;
	err = nds_init(&init_param);
	if (err)
		die("nds_init", err);

	if (register_mem) {
		err = nds_register_mem(hbm.addr, alloc_size, 0);
		if (err) {
			fprintf(stderr,
				"nds_register_mem failed (%d): need device "
				"huge/P2P pages (try --acl-policy 3 or 4, "
				"alloc_size=%zu)\n",
				err, alloc_size);
			die("nds_register_mem", err);
		}
	}

	err = nds_io_new_ctx(&(struct nds_io_ctx_param){ .max_io_cnt = 1 },
			     &ctx);
	if (err)
		die("nds_io_new_ctx", err);

	memset(&cb, 0, sizeof(cb));
	memset(&iov, 0, sizeof(iov));
	iov.buf_addr = (uint64_t)(uintptr_t)hbm.addr;
	iov.buf_len = length;
	cb.opcode = NDS_IO_OP_PREAD;
	cb.rw_flags = register_mem ? NDS_IO_F_REGISTERED_MEM : 0;
	cb.obj.fd = file_fd;
	cb.offset = offset;
	cb.iov = &iov;
	cb.iov_cnt = 1;
	cb.host_pid = 0;
	cb.user_data = 0xC2C2;

	n = nds_io_submit(ctx, 1, &cb);
	if (n != 1)
		die("nds_io_submit", n < 0 ? n : -EIO);

	memset(&ev, 0, sizeof(ev));
	n = nds_io_getevents(ctx, 1, 1, &ev, NULL);
	if (n != 1)
		die("nds_io_getevents", n < 0 ? n : -EIO);
	if (ev.res)
		die("I/O failed", (int)ev.res);

	err = xds_hbm_memcpy_d2h(hbm_buf, hbm.addr, length);
	if (err)
		die("xds_hbm_memcpy_d2h", err);
	hbm_crc = crc32_buf(hbm_buf, length);
	ok = (file_crc == hbm_crc);

	printf("RESULT npu=%d offset=%" PRIu64 " length=%u alloc_size=%zu "
	       "acl_policy=%d register_mem=%d file_crc=0x%08x hbm_crc=0x%08x "
	       "ok=%d\n",
	       npu_device, offset, length, alloc_size, acl_policy,
	       register_mem ? 1 : 0, file_crc, hbm_crc, ok);

	nds_io_destroy_ctx(ctx);
	if (register_mem)
		nds_unregister_mem(hbm.addr, 0, 0);
	nds_exit();
	xds_hbm_free(&hbm);
	free(file_buf);
	free(hbm_buf);
	close(file_fd);
	close(topo_fd);

	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
