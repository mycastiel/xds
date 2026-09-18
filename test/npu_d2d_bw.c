/*
 * npu_d2d_bw — inter-NPU HBM device-to-device memcpy bandwidth.
 *
 * Allocates HBM on --src-device and --dst-device, enables peer access both
 * ways, then times aclrtMemcpy(DEVICE_TO_DEVICE) src → dst. No NVMe / p2p_dev.
 * qd=1 is sync memcpy (current device = dst); qd>1 uses streams on dst.
 *
 * Build: make -C test npu_d2d_bw
 * Example:
 *   source /usr/local/Ascend/ascend-toolkit/set_env.sh
 *   sudo -E ./test/npu_d2d_bw --src-device 0 --dst-device 1 --copy-size 1M \
 *        --queue-depth 1 --total-size 8G --acl-policy 3 --verify
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hbm_acl.h"

#define DEFAULT_COPY (1u << 20)
#define DEFAULT_TOTAL (8ull << 30)
#define DEFAULT_QD 1u
#define WARMUP_ROUNDS 4u

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

static double elapsed_sec(const struct timespec *t0, const struct timespec *t1)
{
	return (double)(t1->tv_sec - t0->tv_sec) +
	       (double)(t1->tv_nsec - t0->tv_nsec) / 1e9;
}

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
	if (sscanf(rp + 2,
		   "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
		   &utime, &stime) != 2)
		return -EINVAL;
	*jiffies_out = (uint64_t)utime + (uint64_t)stime;
	return 0;
}

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

static void barrier_wait(const char *barrier_dir, int rank)
{
	char ready_path[512];
	char go_path[512];
	int fd;

	if (!barrier_dir || !*barrier_dir)
		return;

	snprintf(ready_path, sizeof(ready_path), "%s/ready.%d", barrier_dir,
		 rank);
	snprintf(go_path, sizeof(go_path), "%s/go", barrier_dir);

	fd = open(ready_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		die("barrier ready create", -errno);
	if (write(fd, "1\n", 2) < 0)
		die("barrier ready write", -errno);
	close(fd);

	while (access(go_path, F_OK) != 0)
		usleep(1000);
}

static void fill_pattern(unsigned char *buf, size_t n, unsigned int seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		buf[i] = (unsigned char)(0xA5 ^ (seed + i));
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s --src-device ID --dst-device ID [options]\n"
		"  --src-device ID     source NPU (logical id)\n"
		"  --dst-device ID     destination NPU (must differ from src)\n"
		"  --copy-size BYTES   bytes per D2D memcpy (default 1M)\n"
		"  --queue-depth N     parallel copies / streams on dst (default 1)\n"
		"  --total-size BYTES  aggregate copied bytes (default 8G)\n"
		"  --runtime SEC       timed run; overrides --total-size\n"
		"  --acl-policy N      aclrt malloc policy (default 3=HUGE_FIRST_P2P)\n"
		"  --verify            D2H+memcmp first dst slot after warmup\n"
		"  --barrier-dir DIR   wait until DIR/go\n",
		argv0);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "src-device", required_argument, NULL, 'S' },
		{ "dst-device", required_argument, NULL, 'D' },
		{ "copy-size", required_argument, NULL, 's' },
		{ "queue-depth", required_argument, NULL, 'q' },
		{ "total-size", required_argument, NULL, 'T' },
		{ "runtime", required_argument, NULL, 'U' },
		{ "acl-policy", required_argument, NULL, 'A' },
		{ "verify", no_argument, NULL, 'V' },
		{ "barrier-dir", required_argument, NULL, 'B' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int src_device = -1;
	int dst_device = -1;
	int acl_policy = XDS_ACL_MEM_MALLOC_HUGE_FIRST_P2P;
	uint64_t copy_size = DEFAULT_COPY;
	uint64_t total_size = DEFAULT_TOTAL;
	double runtime_sec = 0;
	bool use_runtime = false;
	uint32_t qd = DEFAULT_QD;
	bool verify = false;
	const char *barrier_dir = NULL;
	struct xds_hbm_buf src_hbm = { 0 };
	struct xds_hbm_buf dst_hbm = { 0 };
	xds_acl_stream_t *streams = NULL;
	unsigned char *host = NULL;
	unsigned char *check = NULL;
	char *src;
	char *dst;
	size_t arena;
	uint32_t i;
	uint32_t w;
	uint64_t issued = 0;
	uint64_t copies = 0;
	uint64_t goal;
	struct timespec t0, t1, now;
	double sec;
	double mib_s;
	long clk_tck;
	uint64_t self_j0 = 0, self_j1 = 0;
	uint64_t sys_busy0 = 0, sys_total0 = 0, sys_busy1 = 0, sys_total1 = 0;
	double proc_cpu_pct = 0, sys_cpu_pct = 0;
	int err;
	int c;

	while ((c = getopt_long(argc, argv, "S:D:s:q:T:U:A:VB:h", opts,
				NULL)) != -1) {
		switch (c) {
		case 'S':
			src_device = atoi(optarg);
			break;
		case 'D':
			dst_device = atoi(optarg);
			break;
		case 's':
			copy_size = parse_size(optarg);
			break;
		case 'q':
			qd = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'T':
			total_size = parse_size(optarg);
			break;
		case 'U':
			runtime_sec = strtod(optarg, NULL);
			use_runtime = true;
			break;
		case 'A':
			acl_policy = atoi(optarg);
			break;
		case 'V':
			verify = true;
			break;
		case 'B':
			barrier_dir = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (src_device < 0 || dst_device < 0 || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (src_device == dst_device)
		die("src-device and dst-device must differ", -EINVAL);
	if (!copy_size || !qd)
		die("copy-size/queue-depth", -EINVAL);
	if (copy_size > SIZE_MAX / qd)
		die("copy-size * qd overflow", -EOVERFLOW);
	arena = (size_t)copy_size * qd;
	if (!use_runtime) {
		if (!total_size)
			die("total-size", -EINVAL);
		goal = (total_size / copy_size) * copy_size;
		if (goal < copy_size)
			die("total-size smaller than copy-size", -EINVAL);
	} else if (runtime_sec <= 0) {
		die("runtime must be > 0", -EINVAL);
	} else {
		goal = UINT64_MAX;
	}

	err = xds_hbm_alloc(&src_hbm, src_device, arena, acl_policy);
	if (err)
		die("xds_hbm_alloc src", err);
	err = xds_hbm_alloc(&dst_hbm, dst_device, arena, acl_policy);
	if (err)
		die("xds_hbm_alloc dst", err);
	src = src_hbm.addr;
	dst = dst_hbm.addr;

	err = xds_hbm_enable_peer(src_device, dst_device);
	if (err)
		die("enable peer src->dst", err);
	err = xds_hbm_enable_peer(dst_device, src_device);
	if (err)
		die("enable peer dst->src", err);

	host = malloc(copy_size);
	if (!host)
		die("malloc host pattern", -ENOMEM);
	fill_pattern(host, copy_size, 1);
	err = xds_hbm_set_device(src_device);
	if (err)
		die("set_device src", err);
	for (i = 0; i < qd; i++) {
		err = xds_hbm_memcpy_h2d(src + (size_t)i * copy_size, host,
					 copy_size);
		if (err)
			die("h2d fill src", err);
	}

	err = xds_hbm_set_device(dst_device);
	if (err)
		die("set_device dst", err);
	if (qd > 1) {
		streams = calloc(qd, sizeof(*streams));
		if (!streams)
			die("calloc streams", -ENOMEM);
		for (i = 0; i < qd; i++) {
			err = xds_hbm_stream_create(&streams[i]);
			if (err)
				die("aclrtCreateStream", err);
		}
	}

	for (w = 0; w < WARMUP_ROUNDS; w++) {
		if (qd == 1) {
			err = xds_hbm_memcpy_d2d(dst, src, copy_size);
			if (err)
				die("warmup d2d", err);
		} else {
			for (i = 0; i < qd; i++) {
				err = xds_hbm_memcpy_async(
					dst + (size_t)i * copy_size,
					src + (size_t)i * copy_size, copy_size,
					XDS_ACL_MEMCPY_DEVICE_TO_DEVICE,
					streams[i]);
				if (err)
					die("warmup d2d async", err);
			}
			for (i = 0; i < qd; i++) {
				err = xds_hbm_stream_synchronize(streams[i]);
				if (err)
					die("warmup sync", err);
			}
		}
	}

	if (verify) {
		check = malloc(copy_size);
		if (!check)
			die("malloc verify", -ENOMEM);
		err = xds_hbm_set_device(dst_device);
		if (err)
			die("set_device dst verify", err);
		err = xds_hbm_memcpy_d2h(check, dst, copy_size);
		if (err)
			die("d2h verify", err);
		if (memcmp(check, host, copy_size))
			die("D2D verify mismatch", -EIO);
		printf("verify=PASS src=%d dst=%d copy_size=%" PRIu64 "\n",
		       src_device, dst_device, copy_size);
		free(check);
		check = NULL;
	}

	err = xds_hbm_set_device(dst_device);
	if (err)
		die("set_device dst timed", err);
	barrier_wait(barrier_dir, src_device * 64 + dst_device);

	clk_tck = sysconf(_SC_CLK_TCK);
	if (clk_tck <= 0)
		clk_tck = 100;
	(void)read_proc_self_jiffies(&self_j0);
	(void)read_proc_stat_cpu(&sys_busy0, &sys_total0);
	if (clock_gettime(CLOCK_MONOTONIC, &t0))
		die("clock_gettime", -errno);

	while (use_runtime || issued < goal) {
		if (use_runtime) {
			if (clock_gettime(CLOCK_MONOTONIC, &now))
				die("clock_gettime", -errno);
			if (elapsed_sec(&t0, &now) >= runtime_sec)
				break;
		} else if (goal - issued < copy_size)
			break;

		if (qd == 1) {
			err = xds_hbm_memcpy_d2d(dst, src, copy_size);
			if (err)
				die("d2d", err);
			issued += copy_size;
			copies++;
		} else {
			uint32_t n = qd;

			if (!use_runtime &&
			    (goal - issued) / copy_size < n)
				n = (uint32_t)((goal - issued) / copy_size);
			if (!n)
				break;
			for (i = 0; i < n; i++) {
				err = xds_hbm_memcpy_async(
					dst + (size_t)i * copy_size,
					src + (size_t)i * copy_size, copy_size,
					XDS_ACL_MEMCPY_DEVICE_TO_DEVICE,
					streams[i]);
				if (err)
					die("d2d async", err);
			}
			for (i = 0; i < n; i++) {
				err = xds_hbm_stream_synchronize(streams[i]);
				if (err)
					die("stream sync", err);
			}
			issued += (uint64_t)n * copy_size;
			copies += n;
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &t1))
		die("clock_gettime", -errno);
	(void)read_proc_self_jiffies(&self_j1);
	(void)read_proc_stat_cpu(&sys_busy1, &sys_total1);

	sec = elapsed_sec(&t0, &t1);
	if (sec <= 0)
		sec = 1e-9;
	mib_s = (issued / (1024.0 * 1024.0)) / sec;
	if (self_j1 >= self_j0)
		proc_cpu_pct = 100.0 * (double)(self_j1 - self_j0) /
			       ((double)clk_tck * sec);
	if (sys_total1 > sys_total0)
		sys_cpu_pct = 100.0 * (double)(sys_busy1 - sys_busy0) /
			      (double)(sys_total1 - sys_total0);

	printf("src=%d dst=%d copy_size=%" PRIu64 " qd=%u acl_policy=%d\n",
	       src_device, dst_device, copy_size, qd, acl_policy);
	printf("transferred=%" PRIu64 " bytes copies=%" PRIu64 " time=%.6f s\n",
	       issued, copies, sec);
	printf("bandwidth=%.2f MiB/s (%.3f GiB/s) copies_s=%.0f\n", mib_s,
	       mib_s / 1024.0, copies / sec);
	printf("RESULT src=%d dst=%d copy_size=%" PRIu64 " qd=%u bytes=%" PRIu64
	       " copies=%" PRIu64 " sec=%.6f mib_s=%.2f iops=%.0f "
	       "proc_cpu_pct=%.2f sys_cpu_pct=%.2f verify=%d verify_ok=1\n",
	       src_device, dst_device, copy_size, qd, issued, copies, sec, mib_s,
	       copies / sec, proc_cpu_pct, sys_cpu_pct, verify ? 1 : 0);

	if (streams) {
		err = xds_hbm_set_device(dst_device);
		if (err)
			die("set_device dst destroy streams", err);
		for (i = 0; i < qd; i++)
			xds_hbm_stream_destroy(streams[i]);
		free(streams);
	}
	free(host);
	xds_hbm_free(&dst_hbm);
	xds_hbm_free(&src_hbm);
	return 0;
}
