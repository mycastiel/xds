/*
 * nds_bw_bench — concurrent NDS PREAD bandwidth measurement.
 *
 * Issues a pipeline of sector-aligned reads (default 32–128 KiB) against a
 * block device or file into CMB/HBM VAs, reaping with nds_io_getevents.
 *
 * Build: make -C test nds_bw_bench
 * Example (VM / stub CMB):
 *   sudo ./test/nds_bw_bench --topology /dev/nvme0n1 --target /dev/nvme0n1 \
 *        --cmb-va 0 --total-size 1G --io-min 32K --io-max 128K --queue-depth 64
 *
 * Example (physical NPU HBM via aclrt):
 *   source /usr/local/Ascend/ascend-toolkit/set_env.sh   # or your CANN env
 *   sudo ./test/nds_bw_bench --topology /dev/nvme0n1 --target /mnt/data/f.bin \
 *        --npu-device 0 --total-size 256M --io-min 128K --io-max 128K --queue-depth 64
 *
 * --total-size is the aggregate bytes transferred (offsets wrap within the
 * target when the device/file is smaller).
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
#include <time.h>
#include <unistd.h>

#include "nds_api.h"
#include "hbm_acl.h"

#define SECTOR 512u
#define DEFAULT_IO_MIN (32u << 10)
#define DEFAULT_IO_MAX (128u << 10)
#define DEFAULT_TOTAL (256ull << 20)
#define DEFAULT_QD 64u

struct slot {
	struct nds_io_cb cb;
	struct nds_io_vec iov;
	uint64_t buf_addr;
	uint32_t length;
	bool busy;
};

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

static uint64_t align_down(uint64_t v, uint64_t a)
{
	return v & ~(a - 1);
}

static uint64_t timespec_ns(const struct timespec *t)
{
	return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
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

static uint32_t pick_io_size(uint32_t io_min, uint32_t io_max, uint64_t remain,
			     unsigned int *rng)
{
	uint32_t span;
	uint32_t n;
	uint32_t sz;

	*rng = *rng * 1103515245u + 12345u;
	if (io_max > remain)
		io_max = (uint32_t)align_down(remain, SECTOR);
	if (io_max < io_min)
		return (uint32_t)align_down(remain, SECTOR);
	span = (io_max - io_min) / SECTOR;
	n = span ? (*rng % (span + 1)) : 0;
	sz = io_min + n * SECTOR;
	if (sz > remain)
		sz = (uint32_t)align_down(remain, SECTOR);
	return sz;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s --topology DEV --target PATH [options]\n"
		"  --cmb-va ADDR       CMB/HBM VA base (default 0; VM stub only)\n"
		"  --npu-device ID      physical: aclrtSetDevice + aclrtMalloc HBM\n"
		"  --acl-policy N      aclrt malloc policy (default 0=HUGE_FIRST)\n"
		"  --total-size BYTES  aggregate bytes to transfer (default 256M; K/M/G ok)\n"
		"  --io-min BYTES      min I/O size (default 32K)\n"
		"  --io-max BYTES      max I/O size (default 128K)\n"
		"  --queue-depth N     outstanding I/Os (default 64)\n"
		"  --passes N          repeat total-size this many times (default 1)\n"
		"  --sequential        sequential offsets, wrap at EOF (default)\n"
		"  --random            random offsets within target\n"
		"  --no-register-mem   one-shot VAs (default: register CMB/HBM window)\n",
		argv0);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "target", required_argument, NULL, 'f' },
		{ "cmb-va", required_argument, NULL, 'a' },
		{ "npu-device", required_argument, NULL, 'd' },
		{ "acl-policy", required_argument, NULL, 'A' },
		{ "total-size", required_argument, NULL, 'T' },
		{ "io-min", required_argument, NULL, 'i' },
		{ "io-max", required_argument, NULL, 'I' },
		{ "queue-depth", required_argument, NULL, 'q' },
		{ "passes", required_argument, NULL, 'p' },
		{ "sequential", no_argument, NULL, 'S' },
		{ "random", no_argument, NULL, 'R' },
		{ "no-register-mem", no_argument, NULL, 'N' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topo_path = NULL;
	const char *target_path = NULL;
	uint64_t cmb_va = 0;
	int npu_device = -1;
	int acl_policy = XDS_ACL_MEM_MALLOC_HUGE_FIRST;
	uint64_t total_size = DEFAULT_TOTAL;
	uint32_t io_min = DEFAULT_IO_MIN;
	uint32_t io_max = DEFAULT_IO_MAX;
	uint32_t qd = DEFAULT_QD;
	unsigned int passes = 1;
	bool random_off = false;
	bool register_mem = true;
	struct xds_hbm_buf hbm = { 0 };
	int topo_fd = -1;
	int file_fd = -1;
	int32_t fs_fds[1];
	struct nds_init_param init_param = {
		.desc = { .fs_fd = fs_fds, .fs_fd_cnt = 1 },
	};
	struct nds_io_ctx *ctx = NULL;
	struct slot *slots = NULL;
	struct nds_io_event *events = NULL;
	struct nds_io_cb *batch_cbs = NULL;
	uint32_t *batch_slots = NULL;
	uint64_t target_bytes;
	uint64_t goal;
	uint64_t completed = 0;
	uint64_t issued = 0;
	uint64_t next_off = 0;
	uint64_t inflight = 0;
	uint64_t ios = 0;
	uint64_t buf_span;
	unsigned int rng = 1;
	struct timespec t0, t1;
	double sec;
	double gib;
	double mib_s;
	int err;
	int c;
	uint32_t i;

	while ((c = getopt_long(argc, argv, "t:f:a:d:A:T:i:I:q:p:SRNh", opts,
				NULL)) != -1) {
		switch (c) {
		case 't':
			topo_path = optarg;
			break;
		case 'f':
			target_path = optarg;
			break;
		case 'a':
			cmb_va = strtoull(optarg, NULL, 0);
			break;
		case 'd':
			npu_device = (int)strtol(optarg, NULL, 0);
			break;
		case 'A':
			acl_policy = (int)strtol(optarg, NULL, 0);
			break;
		case 'T':
			total_size = parse_size(optarg);
			break;
		case 'i':
			io_min = (uint32_t)parse_size(optarg);
			break;
		case 'I':
			io_max = (uint32_t)parse_size(optarg);
			break;
		case 'q':
			qd = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'p':
			passes = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'S':
			random_off = false;
			break;
		case 'R':
			random_off = true;
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

	if (!topo_path || !target_path || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!qd || qd > NDS_IO_MAX_IO_CNT)
		die("queue-depth", -EINVAL);
	if (!io_min || !io_max || io_min > io_max ||
	    (io_min | io_max) & (SECTOR - 1))
		die("io-min/io-max must be non-zero 512B-aligned", -EINVAL);
	if (!total_size || (total_size & (SECTOR - 1)))
		die("total-size must be 512B-aligned", -EINVAL);
	if (!passes)
		die("passes", -EINVAL);

	topo_fd = open(topo_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (topo_fd < 0)
		die("open topology", -errno);
	file_fd = open(target_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (file_fd < 0)
		die("open target", -errno);

	target_bytes = align_down(target_size_bytes(file_fd), SECTOR);
	if (target_bytes < io_min)
		die("target too small", -ENOSPC);
	if (total_size > UINT64_MAX / passes)
		die("total-size * passes overflow", -EOVERFLOW);
	goal = total_size * passes;

	buf_span = (uint64_t)qd * io_max;
	if (npu_device >= 0) {
		err = xds_hbm_alloc(&hbm, npu_device, (size_t)buf_span,
				    acl_policy);
		if (err)
			die("xds_hbm_alloc (aclrt)", err);
		cmb_va = (uint64_t)(uintptr_t)hbm.addr;
	}
	slots = calloc(qd, sizeof(*slots));
	events = calloc(qd, sizeof(*events));
	batch_cbs = calloc(qd, sizeof(*batch_cbs));
	batch_slots = calloc(qd, sizeof(*batch_slots));
	if (!slots || !events || !batch_cbs || !batch_slots)
		die("calloc", -ENOMEM);
	for (i = 0; i < qd; i++) {
		slots[i].buf_addr = cmb_va + (uint64_t)i * io_max;
		slots[i].iov.buf_addr = slots[i].buf_addr;
		slots[i].iov.buf_len = io_max;
	}

	fs_fds[0] = topo_fd;
	err = nds_init(&init_param);
	if (err)
		die("nds_init", err);

	if (register_mem) {
		err = nds_register_mem((void *)(uintptr_t)cmb_va, buf_span, 0);
		if (err)
			die("nds_register_mem", err);
	}

	err = nds_io_new_ctx(&(struct nds_io_ctx_param){ .max_io_cnt = qd },
			     &ctx);
	if (err)
		die("nds_io_new_ctx", err);

	printf("topology=%s target=%s cmb_va=0x%" PRIx64 " npu_device=%d\n",
	       topo_path, target_path, cmb_va, npu_device);
	printf("target_size=%" PRIu64 " (%.2f MiB) total_size=%" PRIu64
	       " passes=%u goal=%" PRIu64 " (%.2f MiB)\n",
	       target_bytes, target_bytes / (1024.0 * 1024.0), total_size,
	       passes, goal, goal / (1024.0 * 1024.0));
	printf("io_size=%u..%u queue_depth=%u register_mem=%d pattern=%s\n",
	       io_min, io_max, qd, register_mem,
	       random_off ? "random" : "sequential");
	fflush(stdout);

	if (clock_gettime(CLOCK_MONOTONIC, &t0))
		die("clock_gettime", -errno);

	while (completed < goal) {
		uint32_t batch_nr = 0;

		while (inflight < qd && issued < goal) {
			uint32_t slot;
			uint32_t len;
			uint64_t off;
			uint64_t remain = goal - issued;

			for (slot = 0; slot < qd; slot++) {
				if (!slots[slot].busy)
					break;
			}
			if (slot >= qd)
				break;

			len = pick_io_size(io_min, io_max, remain, &rng);
			if (!len)
				break;

			if (random_off) {
				uint64_t max_off =
					align_down(target_bytes - len, SECTOR);

				rng = rng * 1103515245u + 12345u;
				if (max_off)
					off = ((uint64_t)rng * SECTOR) %
					      (max_off + SECTOR);
				else
					off = 0;
				off = align_down(off, SECTOR);
			} else {
				if (next_off + len > target_bytes)
					next_off = 0;
				off = next_off;
				next_off += len;
				if (next_off >= target_bytes)
					next_off = 0;
			}

			memset(&slots[slot].cb, 0, sizeof(slots[slot].cb));
			slots[slot].iov.buf_addr = slots[slot].buf_addr;
			slots[slot].iov.buf_len = len;
			slots[slot].length = len;
			slots[slot].cb.opcode = NDS_IO_OP_PREAD;
			slots[slot].cb.rw_flags =
				register_mem ? NDS_IO_F_REGISTERED_MEM : 0;
			slots[slot].cb.obj.fd = file_fd;
			slots[slot].cb.offset = off;
			slots[slot].cb.iov = &slots[slot].iov;
			slots[slot].cb.iov_cnt = 1;
			slots[slot].cb.host_pid = 0;
			slots[slot].cb.user_data = slot;

			batch_cbs[batch_nr] = slots[slot].cb;
			batch_slots[batch_nr] = slot;
			batch_nr++;
			slots[slot].busy = true;
			inflight++;
			issued += len;
		}

		if (batch_nr) {
			int n = nds_io_submit(ctx, (int)batch_nr, batch_cbs);

			if (n != (int)batch_nr) {
				/* Roll back bookkeeping for unsubmitted tail. */
				uint32_t k;

				for (k = (uint32_t)(n < 0 ? 0 : n); k < batch_nr;
				     k++) {
					slots[batch_slots[k]].busy = false;
					inflight--;
					issued -= slots[batch_slots[k]].length;
				}
				die("nds_io_submit", n < 0 ? n : -EIO);
			}
		}

		if (inflight) {
			int got = nds_io_getevents(ctx, 1, (int)inflight,
						   events, NULL);
			int j;

			if (got < 0)
				die("nds_io_getevents", got);
			for (j = 0; j < got; j++) {
				uint32_t slot = (uint32_t)events[j].user_data;

				if (events[j].res) {
					fprintf(stderr,
						"I/O failed slot=%u res=%" PRId64
						"\n",
						slot, events[j].res);
					die("completion", (int)events[j].res);
				}
				if (slot >= qd || !slots[slot].busy)
					die("bad completion cookie", -EINVAL);
				completed += slots[slot].length;
				ios++;
				slots[slot].busy = false;
				inflight--;
			}
		} else if (completed < goal) {
			die("stalled with no inflight I/O", -EIO);
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t1))
		die("clock_gettime", -errno);

	sec = (timespec_ns(&t1) - timespec_ns(&t0)) / 1e9;
	if (sec <= 0)
		sec = 1e-9;
	gib = completed / (1024.0 * 1024.0 * 1024.0);
	mib_s = (completed / (1024.0 * 1024.0)) / sec;

	printf("transferred=%" PRIu64 " bytes (%.3f GiB) ios=%" PRIu64
	       " time=%.6f s\n",
	       completed, gib, ios, sec);
	printf("bandwidth=%.2f MiB/s (%.3f GiB/s) iops=%.0f avg_io=%.1f KiB\n",
	       mib_s, mib_s / 1024.0, ios / sec,
	       ios ? (completed / (double)ios) / 1024.0 : 0.0);
	/* Machine-parseable one-liner for matrix scripts. */
	printf("RESULT pattern=%s register_mem=%d io_min=%u io_max=%u qd=%u "
	       "bytes=%" PRIu64 " ios=%" PRIu64 " sec=%.6f mib_s=%.2f iops=%.0f\n",
	       random_off ? "random" : "sequential", register_mem ? 1 : 0,
	       io_min, io_max, qd, completed, ios, sec, mib_s, ios / sec);

	nds_io_destroy_ctx(ctx);
	if (register_mem)
		nds_unregister_mem((void *)(uintptr_t)cmb_va);
	nds_exit();
	xds_hbm_free(&hbm);
	close(file_fd);
	close(topo_fd);
	free(batch_slots);
	free(batch_cbs);
	free(events);
	free(slots);
	return 0;
}
