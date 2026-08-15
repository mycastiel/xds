/*
 * nds_bw_bench — concurrent NDS PREAD bandwidth measurement.
 *
 * Issues a pipeline of sector-aligned reads against a block device or file
 * into CMB/HBM VAs and reaps with nds_io_getevents. Optional --verify-each
 * D2H+CRC-checks each completion against the file.
 *
 * Build: make -C test nds_bw_bench
 * Example (physical NPU HBM via aclrt):
 *   source /usr/local/Ascend/ascend-toolkit/set_env.sh
 *   sudo -E ./test/nds_bw_bench --topology /dev/nvme0n1 --target /mnt/data/f.bin \
 *        --npu-device 0 --total-size 256M --io-min 128K --io-max 128K \
 *        --queue-depth 64 --verify-each
 *
 * --total-size is the aggregate bytes transferred (offsets wrap within the
 * target when the device/file is smaller), unless --runtime is set.
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "nds_api.h"
#include "hbm_acl.h"

#define SECTOR 512u
#define DEFAULT_IO_MIN (32u << 10)
#define DEFAULT_IO_MAX (128u << 10)
#define DEFAULT_TOTAL (256ull << 20)
#define DEFAULT_QD 64u
#define CRC32_POLY_LE 0xedb88320U

struct slot {
	struct nds_io_cb cb;
	struct nds_io_vec iov;
	uint64_t buf_addr;
	uint64_t offset;
	uint32_t length;
	bool busy;
	bool used;
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

static double elapsed_sec(const struct timespec *t0, const struct timespec *t1)
{
	double sec = (timespec_ns(t1) - timespec_ns(t0)) / 1e9;

	return sec > 0 ? sec : 1e-9;
}

/* /proc/self/stat utime+stime (jiffies). Returns 0 on success. */
static int read_proc_self_jiffies(uint64_t *jiffies_out)
{
	char buf[1024];
	char *rp;
	unsigned long utime = 0;
	unsigned long stime = 0;
	FILE *f;
	size_t n;

	f = fopen("/proc/self/stat", "r");
	if (!f)
		return -errno;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (!n)
		return -EIO;
	buf[n] = '\0';
	rp = strrchr(buf, ')');
	if (!rp)
		return -EINVAL;
	/*
	 * After ") ": state ppid pgrp session tty tpgid flags
	 * minflt cminflt majflt cmajflt utime stime ...
	 */
	if (sscanf(rp + 2,
		   "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
		   &utime, &stime) != 2)
		return -EINVAL;
	*jiffies_out = (uint64_t)utime + (uint64_t)stime;
	return 0;
}

/* Aggregate /proc/stat "cpu " line: busy and total jiffies. */
static int read_proc_stat_cpu(uint64_t *busy_out, uint64_t *total_out)
{
	char line[512];
	unsigned long long user, nice, system, idle, iowait, irq, softirq;
	unsigned long long steal = 0;
	FILE *f;
	uint64_t busy;
	uint64_t total;

	f = fopen("/proc/stat", "r");
	if (!f)
		return -errno;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);
	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user,
		   &nice, &system, &idle, &iowait, &irq, &softirq,
		   &steal) < 7)
		return -EINVAL;
	busy = (uint64_t)user + (uint64_t)nice + (uint64_t)system +
	       (uint64_t)irq + (uint64_t)softirq;
	total = busy + (uint64_t)idle + (uint64_t)iowait + (uint64_t)steal;
	*busy_out = busy;
	*total_out = total;
	return 0;
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

static void barrier_wait(const char *barrier_dir, int npu_device,
			 unsigned int world)
{
	char ready_path[512];
	char go_path[512];
	int fd;

	if (!barrier_dir || !*barrier_dir)
		return;
	(void)world;

	snprintf(ready_path, sizeof(ready_path), "%s/ready.%d", barrier_dir,
		 npu_device >= 0 ? npu_device : getpid());
	snprintf(go_path, sizeof(go_path), "%s/go", barrier_dir);

	fd = open(ready_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		die("barrier ready create", -errno);
	if (write(fd, "1\n", 2) < 0)
		die("barrier ready write", -errno);
	close(fd);

	/* Parent (matrix) creates go after all ranks are ready. */
	while (access(go_path, F_OK) != 0)
		usleep(1000);
}

/* Compare one completed slot's HBM payload to the file range. host must be
 * at least slot->length and 512B-aligned. Returns 0 or -EIO on mismatch.
 */
static int verify_slot_crc(int file_fd, const struct slot *slot, uint32_t slot_id,
			   unsigned char *host)
{
	uint32_t file_crc;
	uint32_t hbm_crc;
	int err;

	if (!slot->length)
		return 0;

	read_file_range(file_fd, slot->offset, slot->length, host);
	file_crc = crc32_buf(host, slot->length);

	err = xds_hbm_memcpy_d2h(host, (void *)(uintptr_t)slot->buf_addr,
				 slot->length);
	if (err)
		die("xds_hbm_memcpy_d2h", err);
	hbm_crc = crc32_buf(host, slot->length);
	if (file_crc != hbm_crc) {
		fprintf(stderr,
			"CRC mismatch slot=%u offset=%" PRIu64
			" length=%u file=0x%08x hbm=0x%08x\n",
			slot_id, slot->offset, slot->length, file_crc, hbm_crc);
		return -EIO;
	}
	return 0;
}

static int verify_last_window_crc(int file_fd, struct slot *slots, uint32_t qd,
				  int npu_device)
{
	unsigned char *host = NULL;
	uint32_t i;
	int mismatches = 0;

	if (npu_device < 0) {
		fprintf(stderr,
			"verify-crc skipped: needs --npu-device for D2H\n");
		return 0;
	}

	for (i = 0; i < qd; i++) {
		if (!slots[i].used || !slots[i].length)
			continue;
		host = aligned_alloc_sector(slots[i].length);
		if (verify_slot_crc(file_fd, &slots[i], i, host))
			mismatches++;
		free(host);
		host = NULL;
	}
	return mismatches ? -EIO : 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s --topology DEV --target PATH [options]\n"
		"  --cmb-va ADDR       CMB/HBM VA base (default 0; VM stub only)\n"
		"  --npu-device ID      physical: aclrtSetDevice + aclrtMalloc HBM\n"
		"  --acl-policy N      aclrt malloc policy (default 0=HUGE_FIRST)\n"
		"  --total-size BYTES  aggregate bytes (default 256M; ignored if --runtime)\n"
		"  --runtime SEC       timed run; stop issuing after SEC seconds\n"
		"  --io-min BYTES      min I/O size (default 32K)\n"
		"  --io-max BYTES      max I/O size (default 128K)\n"
		"  --queue-depth N     outstanding I/Os (default 64)\n"
		"  --passes N          repeat total-size this many times (default 1)\n"
		"  --start-offset N    first target byte (default 0)\n"
		"  --region-size N     sequential/random region size (default: to EOF)\n"
		"  --sequential        sequential offsets, wrap at EOF (default)\n"
		"  --random            random offsets within target\n"
		"  --no-register-mem   one-shot VAs (default: register CMB/HBM window)\n"
		"  --verify-each       after each getevents completion, D2H+CRC vs file\n"
		"                      (needs --npu-device; skips last-window CRC)\n"
		"  --verify-crc        after timing, CRC last-window HBM vs file\n"
		"                      (needs --npu-device)\n"
		"  --no-verify-crc     disable last-window CRC\n"
		"  --barrier-dir DIR   sync start: wait until DIR/go (ready.N files)\n"
		"  --barrier-world N   expected ranks for barrier (default 1)\n",
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
		{ "runtime", required_argument, NULL, 'U' },
		{ "io-min", required_argument, NULL, 'i' },
		{ "io-max", required_argument, NULL, 'I' },
		{ "queue-depth", required_argument, NULL, 'q' },
		{ "passes", required_argument, NULL, 'p' },
		{ "start-offset", required_argument, NULL, 'O' },
		{ "region-size", required_argument, NULL, 'L' },
		{ "sequential", no_argument, NULL, 'S' },
		{ "random", no_argument, NULL, 'R' },
		{ "no-register-mem", no_argument, NULL, 'N' },
		{ "verify-crc", no_argument, NULL, 'V' },
		{ "no-verify-crc", no_argument, NULL, 'X' },
		{ "verify-each", no_argument, NULL, 'E' },
		{ "barrier-dir", required_argument, NULL, 'B' },
		{ "barrier-world", required_argument, NULL, 'W' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topo_path = NULL;
	const char *target_path = NULL;
	const char *barrier_dir = NULL;
	uint64_t cmb_va = 0;
	int npu_device = -1;
	int acl_policy = XDS_ACL_MEM_MALLOC_HUGE_FIRST;
	uint64_t total_size = DEFAULT_TOTAL;
	double runtime_sec = 0;
	bool use_runtime = false;
	uint32_t io_min = DEFAULT_IO_MIN;
	uint32_t io_max = DEFAULT_IO_MAX;
	uint32_t qd = DEFAULT_QD;
	unsigned int passes = 1;
	uint64_t region_start = 0;
	uint64_t region_size = 0;
	uint64_t region_end;
	unsigned int barrier_world = 1;
	bool random_off = false;
	bool register_mem = true;
	bool verify_crc = false;
	bool verify_each = false;
	unsigned char *verify_host = NULL;
	uint64_t crc_checked = 0;
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
	struct timespec t0, t1, now;
	double sec;
	double gib;
	double mib_s;
	double proc_cpu_pct = 0;
	double sys_cpu_pct = 0;
	uint64_t self_j0 = 0;
	uint64_t self_j1 = 0;
	uint64_t sys_busy0 = 0;
	uint64_t sys_total0 = 0;
	uint64_t sys_busy1 = 0;
	uint64_t sys_total1 = 0;
	long clk_tck;
	int err;
	int c;
	uint32_t i;
	int crc_ok = 1;
	bool stop_issue = false;

	while ((c = getopt_long(argc, argv,
				"t:f:a:d:A:T:U:i:I:q:p:O:L:SRNVXEB:W:h",
				opts, NULL)) != -1) {
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
		case 'U':
			runtime_sec = strtod(optarg, NULL);
			use_runtime = true;
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
		case 'O':
			region_start = parse_size(optarg);
			break;
		case 'L':
			region_size = parse_size(optarg);
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
		case 'V':
			verify_crc = true;
			break;
		case 'X':
			verify_crc = false;
			break;
		case 'E':
			verify_each = true;
			break;
		case 'B':
			barrier_dir = optarg;
			break;
		case 'W':
			barrier_world = (unsigned int)strtoul(optarg, NULL, 0);
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
	if (!use_runtime) {
		if (!total_size || (total_size & (SECTOR - 1)))
			die("total-size must be 512B-aligned", -EINVAL);
		if (!passes)
			die("passes", -EINVAL);
	} else if (runtime_sec <= 0)
		die("runtime must be > 0", -EINVAL);
	if (verify_each) {
		if (npu_device < 0)
			die("--verify-each needs --npu-device", -EINVAL);
		/* Per-I/O checks already cover last-window. */
		verify_crc = false;
	} else if (verify_crc && npu_device < 0) {
		die("--verify-crc needs --npu-device", -EINVAL);
	}

	topo_fd = open(topo_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (topo_fd < 0)
		die("open topology", -errno);
	file_fd = open(target_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (file_fd < 0)
		die("open target", -errno);

	target_bytes = align_down(target_size_bytes(file_fd), SECTOR);
	if (target_bytes < io_min)
		die("target too small", -ENOSPC);
	if ((region_start & (SECTOR - 1)) || region_start >= target_bytes)
		die("start-offset must be 512B-aligned and within target",
		    -EINVAL);
	if (!region_size)
		region_size = target_bytes - region_start;
	if ((region_size & (SECTOR - 1)) || region_size > target_bytes ||
	    region_start > target_bytes - region_size)
		die("region-size must be 512B-aligned and within target", -EINVAL);
	if (region_size < io_max)
		die("region-size smaller than io-max", -ENOSPC);
	region_end = region_start + region_size;
	next_off = region_start;
	if (!use_runtime) {
		if (total_size > UINT64_MAX / passes)
			die("total-size * passes overflow", -EOVERFLOW);
		goal = total_size * passes;
	} else {
		goal = UINT64_MAX;
	}

	buf_span = (uint64_t)qd * io_max;
	if (buf_span < 1048576)
		buf_span = 1048576;
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
	if (verify_each)
		verify_host = aligned_alloc_sector(io_max);

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
	if (use_runtime)
		printf("target_size=%" PRIu64 " (%.2f MiB) runtime=%.3f s\n",
		       target_bytes, target_bytes / (1024.0 * 1024.0),
		       runtime_sec);
	else
		printf("target_size=%" PRIu64 " (%.2f MiB) total_size=%" PRIu64
		       " passes=%u goal=%" PRIu64 " (%.2f MiB)\n",
		       target_bytes, target_bytes / (1024.0 * 1024.0),
		       total_size, passes, goal, goal / (1024.0 * 1024.0));
	printf("io_size=%u..%u queue_depth=%u register_mem=%d pattern=%s "
	       "region=%" PRIu64 "..%" PRIu64 " "
	       "verify_crc=%d verify_each=%d\n",
	       io_min, io_max, qd, register_mem,
	       random_off ? "random" : "sequential", region_start, region_end,
	       verify_crc ? 1 : 0,
	       verify_each ? 1 : 0);
	fflush(stdout);

	barrier_wait(barrier_dir, npu_device, barrier_world);

	clk_tck = sysconf(_SC_CLK_TCK);
	if (clk_tck <= 0)
		clk_tck = 100;
	(void)read_proc_self_jiffies(&self_j0);
	(void)read_proc_stat_cpu(&sys_busy0, &sys_total0);

	if (clock_gettime(CLOCK_MONOTONIC, &t0))
		die("clock_gettime", -errno);

	while (!stop_issue || inflight > 0) {
		uint32_t batch_nr = 0;
		uint64_t round_bytes = 0;

		if (use_runtime && !stop_issue) {
			if (clock_gettime(CLOCK_MONOTONIC, &now))
				die("clock_gettime", -errno);
			if (elapsed_sec(&t0, &now) >= runtime_sec)
				stop_issue = true;
		}
		if (!use_runtime && completed >= goal && inflight == 0)
			break;
		if (!use_runtime && issued >= goal)
			stop_issue = true;

		while (!stop_issue && inflight < qd &&
		       (use_runtime || issued < goal)) {
			uint32_t slot;
			uint32_t len;
			uint64_t off;
			uint64_t remain;

			if (use_runtime)
				remain = io_max;
			else
				remain = goal - issued;

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
					align_down(region_size - len, SECTOR);

				rng = rng * 1103515245u + 12345u;
				if (max_off)
					off = region_start +
					      ((uint64_t)rng * SECTOR) %
					      (max_off + SECTOR);
				else
					off = region_start;
				off = align_down(off, SECTOR);
			} else {
				if (next_off + len > region_end)
					next_off = region_start;
				off = next_off;
				next_off += len;
				if (next_off >= region_end)
					next_off = region_start;
			}

			memset(&slots[slot].cb, 0, sizeof(slots[slot].cb));
			slots[slot].iov.buf_addr = slots[slot].buf_addr;
			slots[slot].iov.buf_len = len;
			slots[slot].length = len;
			slots[slot].offset = off;
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
			slots[slot].used = true;
			inflight++;
			issued += len;
			round_bytes += len;
		}

		if (batch_nr) {
			int n = nds_io_submit(ctx, (int)batch_nr, batch_cbs);

			if (n != (int)batch_nr) {
				uint32_t k;

				for (k = (uint32_t)(n < 0 ? 0 : n); k < batch_nr;
				     k++) {
					slots[batch_slots[k]].busy = false;
					inflight--;
					issued -= slots[batch_slots[k]].length;
					round_bytes -=
						slots[batch_slots[k]].length;
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
				if (verify_each) {
					err = verify_slot_crc(file_fd,
							      &slots[slot],
							      slot,
							      verify_host);
					if (err)
						die("verify-each CRC", err);
					crc_checked++;
				}
				completed += slots[slot].length;
				ios++;
				slots[slot].busy = false;
				inflight--;
			}
		} else if (!stop_issue && (use_runtime || completed < goal)) {
			die("stalled with no inflight I/O", -EIO);
		}

		if (!use_runtime && completed >= goal && inflight == 0)
			break;
		(void)round_bytes;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t1))
		die("clock_gettime", -errno);
	(void)read_proc_self_jiffies(&self_j1);
	(void)read_proc_stat_cpu(&sys_busy1, &sys_total1);

	if (use_runtime)
		sec = runtime_sec > 0 ? runtime_sec : elapsed_sec(&t0, &t1);
	else
		sec = elapsed_sec(&t0, &t1);
	gib = completed / (1024.0 * 1024.0 * 1024.0);
	mib_s = (completed / (1024.0 * 1024.0)) / sec;
	if (self_j1 >= self_j0 && sec > 0)
		proc_cpu_pct = 100.0 * (double)(self_j1 - self_j0) /
			       ((double)clk_tck * sec);
	if (sys_total1 > sys_total0) {
		uint64_t db = sys_busy1 - sys_busy0;
		uint64_t dt = sys_total1 - sys_total0;

		sys_cpu_pct = 100.0 * (double)db / (double)dt;
	}

	printf("transferred=%" PRIu64 " bytes (%.3f GiB) ios=%" PRIu64
	       " time=%.6f s\n",
	       completed, gib, ios, sec);
	printf("bandwidth=%.2f MiB/s (%.3f GiB/s) iops=%.0f avg_io=%.1f KiB\n",
	       mib_s, mib_s / 1024.0, ios / sec,
	       ios ? (completed / (double)ios) / 1024.0 : 0.0);
	printf("cpu proc=%.2f%% (1-core) sys=%.2f%% (machine)\n", proc_cpu_pct,
	       sys_cpu_pct);

	if (verify_crc) {
		err = verify_last_window_crc(file_fd, slots, qd, npu_device);
		crc_ok = (err == 0);
		printf("verify_crc last-window %s\n",
		       crc_ok ? "PASS" : "FAIL");
	} else if (verify_each) {
		printf("verify_each checked=%" PRIu64 " %s\n", crc_checked,
		       crc_ok ? "PASS" : "FAIL");
	}

	printf("RESULT pattern=%s register_mem=%d io_min=%u io_max=%u qd=%u "
	       "region_start=%" PRIu64 " region_size=%" PRIu64 " "
	       "verify_crc=%d verify_each=%d crc_checked=%" PRIu64
	       " crc_ok=%d bytes=%" PRIu64 " ios=%" PRIu64
	       " sec=%.6f mib_s=%.2f iops=%.0f proc_cpu_pct=%.2f "
	       "sys_cpu_pct=%.2f\n",
	       random_off ? "random" : "sequential", register_mem ? 1 : 0,
	       io_min, io_max, qd, region_start, region_size,
	       verify_crc ? 1 : 0, verify_each ? 1 : 0,
	       crc_checked, crc_ok, completed, ios, sec, mib_s, ios / sec,
	       proc_cpu_pct, sys_cpu_pct);

	nds_io_destroy_ctx(ctx);
	if (register_mem)
		nds_unregister_mem((void *)(uintptr_t)cmb_va, 0, 0);
	nds_exit();
	xds_hbm_free(&hbm);
	close(file_fd);
	close(topo_fd);
	free(verify_host);
	free(batch_slots);
	free(batch_cbs);
	free(events);
	free(slots);
	return crc_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
