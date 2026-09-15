/*
 * Confirm the registered-memory release D-state:
 *   p2p_release -> __fput -> do_exit
 *
 * Memory is registered on one p2p fd while I/O that holds io_refs runs on a
 * second fd. Waiting in p2p_release for those refs blocks close_files()
 * during do_exit (D-state deadlock). Unpin must be async after the last put.
 *
 * --mode close  close(owner_fd) with I/O still in flight (default)
 * --mode exit   _exit() so do_exit/close_files runs p2p_release
 *
 * --expect-hang  fail unless D + p2p_release is held for 500ms (default)
 * --expect-ok    fail if that wait is seen; child must exit within 60s
 *
 * Build the module with -DCALC_CRC32 so completion-path CRC keeps io_refs
 * alive long enough to sample /proc/<pid>/stack.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "file_p2p_api.h"

#define REG_ADDR 0UL
#define IO_SIZE (8UL << 20)
#define IO_COUNT 4U
#define REG_SIZE (IO_SIZE * IO_COUNT)
#define HANG_HOLD_NS (500ULL * 1000 * 1000)
#define HANG_POLL_NS (10ULL * 1000 * 1000 * 1000)
#define OK_WAIT_NS (60ULL * 1000 * 1000 * 1000)
#define REAP_AFTER_HANG_NS (10ULL * 1000 * 1000 * 1000)

enum test_mode {
	MODE_CLOSE,
	MODE_EXIT,
};

static void die(const char *what, int err)
{
	fprintf(stderr, "release_dstate_test: %s: %s (%d)\n", what,
		err < 0 ? strerror(-err) : strerror(errno),
		err < 0 ? -err : errno);
	_exit(EXIT_FAILURE);
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		die("clock_gettime", -errno);
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static ssize_t read_proc(const char *path, char *buf, size_t buf_size)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, buf_size - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	return n;
}

static bool proc_state_is_d(pid_t pid)
{
	char path[64];
	char buf[4096];
	const char *line;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	if (read_proc(path, buf, sizeof(buf)) < 0)
		return false;
	line = strstr(buf, "State:\t");
	return line && line[7] == 'D';
}

static bool stack_shows_release_wait(pid_t pid, enum test_mode mode,
				     char *stack, size_t stack_size)
{
	char path[64];
	char wchan[256];
	bool has_release;

	snprintf(path, sizeof(path), "/proc/%d/stack", pid);
	if (read_proc(path, stack, stack_size) < 0)
		stack[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/wchan", pid);
	if (read_proc(path, wchan, sizeof(wchan)) < 0)
		wchan[0] = '\0';
	if (wchan[0] && strlen(stack) + strlen(wchan) + 16 < stack_size)
		snprintf(stack + strlen(stack), stack_size - strlen(stack),
			 "\nwchan=%s\n", wchan);

	/*
	 * Drain of this fd's own I/O is not the registered-memory deadlock.
	 * The bug was waiting in p2p_release for other fds' io_refs.
	 */
	if (strstr(stack, "p2p_drain_io") ||
	    strstr(stack, "percpu_ref_switch_to_atomic"))
		return false;

	has_release = strstr(stack, "p2p_release") ||
		      strstr(stack, "p2p_destroy_registered_mem") ||
		      strstr(wchan, "p2p_release") ||
		      strstr(wchan, "p2p_destroy");
	if (!has_release)
		return false;

	if (mode == MODE_EXIT) {
		if (strstr(stack, "do_exit") || strstr(stack, "exit_task_work") ||
		    strstr(stack, "task_work_run"))
			return true;
		if (!strstr(stack, "__fput") && !strstr(stack, "____fput"))
			return false;
	}

	return strstr(stack, "__fput") || strstr(stack, "do_exit") ||
	       strstr(stack, "wait_for_completion") ||
	       strstr(stack, "wait_for_common") ||
	       strstr(wchan, "p2p_release") ||
	       strstr(stack, "p2p_destroy_registered_mem");
}

static int waitpid_until(pid_t pid, uint64_t deadline_ns, int *status)
{
	for (;;) {
		pid_t got = waitpid(pid, status, WNOHANG);

		if (got == pid)
			return 0;
		if (got < 0)
			return -errno;
		if (monotonic_ns() >= deadline_ns)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

static int submit_inflight_reads(int io_fd, const char *source, uint64_t handle)
{
	unsigned int i;
	int err;

	for (i = 0; i < IO_COUNT; i++) {
		struct p2p_iov iov = {
			.addr = REG_ADDR + (unsigned long)i * IO_SIZE,
			.size = IO_SIZE,
		};
		struct io_parameter param = {
			.op = P2P_IO_READ,
			.file_name = source,
			.file_offset = (unsigned long)i * IO_SIZE,
			.iov = &iov,
			.iov_nr = 1,
			.flags = P2P_IO_F_REGISTERED_MEM,
			.mem_handle = handle,
		};

		err = rw_file(io_fd, &param);
		if (err)
			return err;
	}
	return 0;
}

static void child_submit(const char *topology, int ready_fd, enum test_mode mode)
{
	struct p2p_mem_register_param reg = {
		.addr = REG_ADDR,
		.size = REG_SIZE,
	};
	char go = 'x';
	int owner_fd;
	int io_fd;
	int err;

	owner_fd = new_p2p_fd();
	io_fd = new_p2p_fd();
	if (owner_fd < 0)
		die("open owner p2p fd", owner_fd);
	if (io_fd < 0)
		die("open io p2p fd", io_fd);
	if (owner_fd >= io_fd)
		die("owner fd must be the lower number", -EINVAL);

	err = add_topo(owner_fd, topology);
	if (err)
		die("add_topo owner", err);
	err = add_topo(io_fd, topology);
	if (err)
		die("add_topo io", err);
	err = register_mem(owner_fd, &reg);
	if (err)
		die("register_mem", err);
	if (!reg.mem_handle)
		die("register_mem handle", -EINVAL);

	err = submit_inflight_reads(io_fd, topology, reg.mem_handle);
	if (err)
		die("submit registered reads", err);

	if (write(ready_fd, &go, 1) != 1)
		die("signal parent", -errno);
	close(ready_fd);

	if (mode == MODE_EXIT) {
		/*
		 * Leave both fds open. do_exit -> exit_files -> fput runs
		 * p2p_release on the owner fd while io_refs are still held.
		 */
		_exit(0);
	}

	/*
	 * close() runs ____fput with mm still live, so p2p_release waits
	 * here on the sync destroy path.
	 */
	if (close(owner_fd))
		die("close owner fd", -errno);
	drain_io(io_fd);
	close(io_fd);
	_exit(0);
}

static int run_child(const char *topology, enum test_mode mode, pid_t *pid_out)
{
	int ready[2];
	pid_t pid;
	char go;
	ssize_t n;

	if (pipe(ready))
		return -errno;
	pid = fork();
	if (pid < 0) {
		close(ready[0]);
		close(ready[1]);
		return -errno;
	}
	if (!pid) {
		close(ready[0]);
		child_submit(topology, ready[1], mode);
	}
	close(ready[1]);
	n = read(ready[0], &go, 1);
	close(ready[0]);
	if (n != 1) {
		waitpid(pid, NULL, 0);
		return n < 0 ? -errno : -EPIPE;
	}
	*pid_out = pid;
	return 0;
}

static int expect_hang(pid_t pid, enum test_mode mode)
{
	char stack[8192] = "";
	char last[8192] = "";
	uint64_t deadline = monotonic_ns() + HANG_POLL_NS;
	uint64_t held_since = 0;
	bool seen = false;
	int status;
	int err;

	while (monotonic_ns() < deadline) {
		uint64_t now = monotonic_ns();
		bool in_d = proc_state_is_d(pid);
		bool wait = stack_shows_release_wait(pid, mode, stack,
						     sizeof(stack));

		if (stack[0])
			snprintf(last, sizeof(last), "D=%d\n%s", in_d ? 1 : 0,
				 stack);
		if (in_d && wait) {
			if (!held_since)
				held_since = now;
			if (now - held_since >= HANG_HOLD_NS) {
				seen = true;
				printf("reproduced D-state in pid %d\n%s\n", pid,
				       stack);
				break;
			}
		} else {
			held_since = 0;
		}
		if (waitpid(pid, &status, WNOHANG) == pid) {
			fprintf(stderr,
				"child exited before D-state was sampled (status=%d)\n"
				"last /proc snapshot:\n%s\n",
				status, last);
			return -EAGAIN;
		}
		usleep(1000);
	}
	if (!seen) {
		fprintf(stderr,
			"did not observe D-state in p2p_release; rebuild with "
			"KCFLAGS=-DCALC_CRC32\n"
			"last /proc snapshot:\n%s\n",
			last);
		waitpid(pid, NULL, WNOHANG);
		return -ETIMEDOUT;
	}

	err = waitpid_until(pid, monotonic_ns() + REAP_AFTER_HANG_NS, &status);
	if (err) {
		printf("child remains in D after reproducing hang (deadlock or "
		       "still waiting for io_refs)\n");
		return 0;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child finished with status %d\n", status);
		return -EIO;
	}
	printf("release hang reproduced, child later exited 0\n");
	return 0;
}

static int expect_ok(pid_t pid, enum test_mode mode)
{
	char stack[8192];
	uint64_t deadline = monotonic_ns() + OK_WAIT_NS;
	uint64_t held_since = 0;
	int status;
	int err;

	while (monotonic_ns() < deadline) {
		uint64_t now = monotonic_ns();

		if (proc_state_is_d(pid) &&
		    stack_shows_release_wait(pid, mode, stack, sizeof(stack))) {
			if (!held_since)
				held_since = now;
			if (now - held_since >= HANG_HOLD_NS) {
				fprintf(stderr,
					"release still blocked in D:\n%s\n",
					stack);
				waitpid_until(pid,
					      monotonic_ns() + REAP_AFTER_HANG_NS,
					      &status);
				return -EDEADLK;
			}
		} else {
			held_since = 0;
		}
		err = waitpid(pid, &status, WNOHANG);
		if (err == pid) {
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				fprintf(stderr, "child status %d\n", status);
				return -EIO;
			}
			printf("child reaped without p2p_release D-state wait\n");
			return 0;
		}
		if (err < 0)
			return -errno;
		usleep(1000);
	}
	fprintf(stderr, "child was not reaped within 60s\n");
	return -ETIMEDOUT;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --topology <block-device> [--mode close|exit] "
		"[--expect-hang|--expect-ok]\n",
		program);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "mode", required_argument, NULL, 'm' },
		{ "expect-hang", no_argument, NULL, 'H' },
		{ "expect-ok", no_argument, NULL, 'O' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topology = NULL;
	enum test_mode mode = MODE_CLOSE;
	bool want_hang = true;
	bool mode_set = false;
	bool expect_set = false;
	pid_t pid = 0;
	int option;
	int err;

	while ((option = getopt_long(argc, argv, "t:m:", options, NULL)) != -1) {
		switch (option) {
		case 't':
			topology = optarg;
			break;
		case 'm':
			if (!strcmp(optarg, "close"))
				mode = MODE_CLOSE;
			else if (!strcmp(optarg, "exit"))
				mode = MODE_EXIT;
			else {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			mode_set = true;
			break;
		case 'H':
			want_hang = true;
			expect_set = true;
			break;
		case 'O':
			want_hang = false;
			expect_set = true;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	(void)mode_set;
	(void)expect_set;
	if (!topology || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	err = run_child(topology, mode, &pid);
	if (err) {
		fprintf(stderr, "failed to start child: %s\n", strerror(-err));
		return EXIT_FAILURE;
	}
	printf("child pid %d mode=%s %u x %luKiB registered reads\n", pid,
	       mode == MODE_EXIT ? "exit" : "close", IO_COUNT, IO_SIZE / 1024);
	err = want_hang ? expect_hang(pid, mode) : expect_ok(pid, mode);
	if (err) {
		fprintf(stderr, "release_dstate_test failed: %s\n",
			strerror(-err));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
