/*
 * Registered-memory lifetime edges that the older suites never combined:
 * owner fd != I/O fd, revoke/close/_exit while io_refs are live, then
 * stale-handle and cleanup checks. Success is printed only after every fd
 * is closed so a D-state hang in p2p_release cannot look like a pass.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
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
#define IO_SIZE (64UL << 10)
#define IO_COUNT 8U
#define REG_SIZE (IO_SIZE * IO_COUNT)
#define SECOND_ADDR (REG_SIZE)
#define SECOND_SIZE IO_SIZE
#define CHILD_WAIT_NS (60ULL * 1000 * 1000 * 1000)
#define HANG_HOLD_NS (500ULL * 1000 * 1000)

struct fds {
	int owner;
	int io;
	int extra[4];
	unsigned int extra_nr;
};

struct close_worker {
	int fd;
	int result;
};

static void die(const char *what, int err)
{
	fprintf(stderr, "regmem_lifetime_test: %s: %s (%d)\n", what,
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

static int expect(const char *operation, int actual, int expected)
{
	printf("api=c case=%s ret=%d expected=%d\n", operation, actual, expected);
	if (actual == expected)
		return 0;
	fprintf(stderr, "%s returned %d, expected %d\n", operation, actual,
		expected);
	return -EINVAL;
}

static int expect_one_of(const char *operation, int actual, int a, int b)
{
	printf("api=c case=%s ret=%d expected=%d|%d\n", operation, actual, a, b);
	if (actual == a || actual == b)
		return 0;
	fprintf(stderr, "%s returned %d, expected %d or %d\n", operation, actual,
		a, b);
	return -EINVAL;
}

static int register_range(int fd, unsigned long addr, unsigned long size,
			  uint64_t *handle)
{
	struct p2p_mem_register_param param = {
		.addr = addr,
		.size = size,
	};
	int err;

	err = register_mem(fd, &param);
	if (err)
		return err;
	if (!param.mem_handle)
		return -EINVAL;
	*handle = param.mem_handle;
	return 0;
}

static int unregister_handle(int fd, uint64_t handle)
{
	struct p2p_mem_unregister_param param = {
		.mem_handle = handle,
	};

	return unregister_mem(fd, &param);
}

static int submit_reads(int io_fd, const char *source, uint64_t handle,
			unsigned int count)
{
	unsigned int i;
	int err;

	for (i = 0; i < count; i++) {
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

static int submit_one(int io_fd, const char *source, uint64_t handle,
		      unsigned long addr, unsigned long size)
{
	struct p2p_iov iov = {
		.addr = addr,
		.size = size,
	};
	struct io_parameter param = {
		.op = P2P_IO_READ,
		.file_name = source,
		.file_offset = 0,
		.iov = &iov,
		.iov_nr = 1,
		.flags = P2P_IO_F_REGISTERED_MEM,
		.mem_handle = handle,
	};

	return rw_file(io_fd, &param);
}

static void fds_init(struct fds *fds)
{
	unsigned int i;

	fds->owner = -1;
	fds->io = -1;
	fds->extra_nr = 0;
	for (i = 0; i < 4; i++)
		fds->extra[i] = -1;
}

static void fds_close(struct fds *fds)
{
	unsigned int i;

	for (i = 0; i < fds->extra_nr; i++) {
		if (fds->extra[i] >= 0) {
			close_p2p_fd(fds->extra[i]);
			fds->extra[i] = -1;
		}
	}
	fds->extra_nr = 0;
	if (fds->io >= 0) {
		close_p2p_fd(fds->io);
		fds->io = -1;
	}
	if (fds->owner >= 0) {
		close_p2p_fd(fds->owner);
		fds->owner = -1;
	}
}

static int fds_open_pair(struct fds *fds, const char *topology)
{
	int err;

	fds->owner = new_p2p_fd();
	fds->io = new_p2p_fd();
	if (fds->owner < 0)
		return fds->owner;
	if (fds->io < 0)
		return fds->io;
	err = add_topo(fds->owner, topology);
	if (err)
		return err;
	return add_topo(fds->io, topology);
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

static bool proc_state_is_d(pid_t pid)
{
	char path[64];
	char buf[4096];
	const char *line;
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return false;
	buf[n] = '\0';
	line = strstr(buf, "State:\t");
	return line && line[7] == 'D';
}

static bool stack_shows_release_wait(pid_t pid)
{
	char path[64];
	char stack[8192];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/proc/%d/stack", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, stack, sizeof(stack) - 1);
	close(fd);
	if (n < 0)
		return false;
	stack[n] = '\0';
	if (strstr(stack, "p2p_drain_io"))
		return false;
	return strstr(stack, "p2p_release") &&
	       (strstr(stack, "wait_for_completion") ||
		strstr(stack, "wait_for_common") ||
		strstr(stack, "p2p_destroy_registered_mem"));
}

static int reap_child_no_release_hang(pid_t pid)
{
	uint64_t deadline = monotonic_ns() + CHILD_WAIT_NS;
	uint64_t held_since = 0;
	int status;
	int err;

	while (monotonic_ns() < deadline) {
		if (proc_state_is_d(pid) && stack_shows_release_wait(pid)) {
			if (!held_since)
				held_since = monotonic_ns();
			if (monotonic_ns() - held_since >= HANG_HOLD_NS) {
				fprintf(stderr, "child %d stuck in p2p_release D-state\n",
					pid);
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
			return 0;
		}
		if (err < 0)
			return -errno;
		usleep(1000);
	}
	waitpid_until(pid, monotonic_ns() + 1000, &status);
	return -ETIMEDOUT;
}

static void *close_owner_thread(void *argument)
{
	struct close_worker *worker = argument;

	worker->result = close_p2p_fd(worker->fd);
	return NULL;
}

static int case_close_owner_inflight(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	err = close_p2p_fd(fds.owner);
	fds.owner = -1;
	if (err)
		goto out;
	err = expect_one_of("stale submit after owner close",
			    submit_one(fds.io, topology, handle, REG_ADDR,
				       IO_SIZE),
			    -ENOENT, -ESTALE);
	if (err)
		goto out;
	err = drain_io(fds.io);

out:
	fds_close(&fds);
	return err;
}

static int case_unregister_inflight(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	err = expect("unregister while inflight",
		     unregister_handle(fds.owner, handle), 0);
	if (err)
		goto out;
	err = expect_one_of("stale submit after unregister",
			    submit_one(fds.io, topology, handle, REG_ADDR,
				       IO_SIZE),
			    -ENOENT, -ESTALE);
	if (err)
		goto out;
	err = drain_io(fds.io);
	if (err)
		goto out;
	err = expect("double unregister", unregister_handle(fds.owner, handle),
		     -ENOENT);

out:
	fds_close(&fds);
	return err;
}

static int case_close_io_first(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	err = close_p2p_fd(fds.io);
	fds.io = -1;
	if (err)
		goto out;
	err = expect("owner still unregisters after io close",
		     unregister_handle(fds.owner, handle), 0);

out:
	fds_close(&fds);
	return err;
}

static int case_idle_close(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = close_p2p_fd(fds.owner);
	fds.owner = -1;
	if (err)
		goto out;
	err = expect("idle handle gone",
		     submit_one(fds.io, topology, handle, REG_ADDR, IO_SIZE),
		     -ENOENT);

out:
	fds_close(&fds);
	return err;
}

static int case_multi_mem_revoke(const char *topology)
{
	struct fds fds;
	uint64_t first = 0;
	uint64_t second = 0;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &first);
	if (err)
		goto out;
	err = register_range(fds.owner, SECOND_ADDR, SECOND_SIZE, &second);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, first, IO_COUNT);
	if (err)
		goto out;
	err = submit_one(fds.io, topology, second, SECOND_ADDR, SECOND_SIZE);
	if (err)
		goto out;
	err = close_p2p_fd(fds.owner);
	fds.owner = -1;
	if (err)
		goto out;
	err = expect("first handle revoked",
		     submit_one(fds.io, topology, first, REG_ADDR, IO_SIZE),
		     -ENOENT);
	if (err)
		goto out;
	err = expect("second handle revoked",
		     submit_one(fds.io, topology, second, SECOND_ADDR,
				SECOND_SIZE),
		     -ENOENT);
	if (err)
		goto out;
	err = drain_io(fds.io);

out:
	fds_close(&fds);
	return err;
}

static int case_reregister_after_close(const char *topology)
{
	struct fds fds;
	uint64_t old_handle = 0;
	uint64_t new_handle = 0;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &old_handle);
	if (err)
		goto out;
	err = close_p2p_fd(fds.owner);
	fds.owner = -1;
	if (err)
		goto out;
	fds.owner = new_p2p_fd();
	if (fds.owner < 0) {
		err = fds.owner;
		goto out;
	}
	err = add_topo(fds.owner, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &new_handle);
	if (err)
		goto out;
	if (new_handle == old_handle) {
		fprintf(stderr, "handle reused after close: 0x%llx\n",
			(unsigned long long)old_handle);
		err = -EINVAL;
		goto out;
	}
	err = expect("stale handle after reregister",
		     submit_one(fds.io, topology, old_handle, REG_ADDR,
				IO_SIZE),
		     -ENOENT);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, new_handle, IO_COUNT);
	if (err)
		goto out;
	err = drain_io(fds.io);
	if (err)
		goto out;
	err = unregister_handle(fds.owner, new_handle);

out:
	fds_close(&fds);
	return err;
}

static int case_many_io_fds(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	unsigned int i;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	for (i = 0; i < 4; i++) {
		fds.extra[i] = new_p2p_fd();
		if (fds.extra[i] < 0) {
			err = fds.extra[i];
			goto out;
		}
		err = add_topo(fds.extra[i], topology);
		if (err)
			goto out;
		fds.extra_nr++;
	}
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	for (i = 0; i < fds.extra_nr; i++) {
		err = submit_one(fds.extra[i], topology, handle,
				 REG_ADDR + (unsigned long)i * IO_SIZE,
				 IO_SIZE);
		if (err)
			goto out;
	}
	err = close_p2p_fd(fds.owner);
	fds.owner = -1;
	if (err)
		goto out;
	err = drain_io(fds.io);
	if (err)
		goto out;
	for (i = 0; i < fds.extra_nr; i++) {
		err = drain_io(fds.extra[i]);
		if (err)
			goto out;
	}

out:
	fds_close(&fds);
	return err;
}

static int case_close_races_submit(const char *topology)
{
	struct fds fds;
	struct close_worker worker = { .fd = -1, .result = -EINPROGRESS };
	pthread_t thread;
	uint64_t handle;
	unsigned int i;
	int err;
	int join_err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	worker.fd = fds.owner;
	if (pthread_create(&thread, NULL, close_owner_thread, &worker)) {
		err = -ENOMEM;
		goto out;
	}
	fds.owner = -1;
	for (i = 0; i < IO_COUNT; i++) {
		int submit_err;

		submit_err = submit_one(fds.io, topology, handle,
					REG_ADDR + (unsigned long)i * IO_SIZE,
					IO_SIZE);
		if (submit_err && submit_err != -ENOENT &&
		    submit_err != -ESTALE) {
			err = submit_err;
			break;
		}
	}
	join_err = pthread_join(thread, NULL);
	if (join_err && !err)
		err = -join_err;
	if (!err)
		err = worker.result;
	if (!err)
		err = drain_io(fds.io);

out:
	fds_close(&fds);
	return err;
}

static int run_child_exit_or_kill(const char *topology, bool use_kill)
{
	pid_t pid;
	int ready[2];
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
		struct fds fds;
		uint64_t handle;
		int err;

		close(ready[0]);
		fds_init(&fds);
		err = fds_open_pair(&fds, topology);
		if (err)
			die("child open", err);
		if (fds.owner >= fds.io)
			die("owner fd must be lower", -EINVAL);
		err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
		if (err)
			die("child register", err);
		err = submit_reads(fds.io, topology, handle, IO_COUNT);
		if (err)
			die("child submit", err);
		if (write(ready[1], "x", 1) != 1)
			die("child ready", -errno);
		close(ready[1]);
		if (use_kill) {
			for (;;)
				pause();
		}
		_exit(0);
	}
	close(ready[1]);
	n = read(ready[0], &go, 1);
	close(ready[0]);
	if (n != 1) {
		waitpid(pid, NULL, 0);
		return n < 0 ? -errno : -EPIPE;
	}
	if (use_kill && kill(pid, SIGKILL)) {
		waitpid(pid, NULL, 0);
		return -errno;
	}
	if (use_kill) {
		int status;
		int err;

		err = waitpid_until(pid, monotonic_ns() + CHILD_WAIT_NS, &status);
		if (err)
			return err;
		if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
			fprintf(stderr, "SIGKILL child status %d\n", status);
			return -EIO;
		}
		return 0;
	}
	return reap_child_no_release_hang(pid);
}

struct named_case {
	const char *name;
	int (*run)(const char *topology);
};

static int read_sysfs_int(const char *path, int *value)
{
	char buf[32];
	int fd;
	ssize_t n;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	*value = atoi(buf);
	return 0;
}

static int stub_notify_free(unsigned long addr, unsigned long size)
{
	char buf[128];
	int fd;
	int n;
	ssize_t wrote;

	n = snprintf(buf, sizeof(buf), "0x%lx 0x%lx\n", addr, size);
	if (n < 0 || n >= (int)sizeof(buf))
		return -EINVAL;
	fd = open("/sys/module/stub/parameters/notify_free", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	wrote = write(fd, buf, (size_t)n);
	close(fd);
	if (wrote != n)
		return wrote < 0 ? -errno : -EIO;
	return 0;
}

static int stub_notify_cb_calls(void)
{
	int value = 0;
	int err;

	err = read_sysfs_int("/sys/module/stub/parameters/notify_cb_calls", &value);
	if (err)
		return err;
	return value;
}

static int submit_oneshot(int io_fd, const char *source, unsigned long addr,
			  unsigned long size)
{
	struct p2p_iov iov = {
		.addr = addr,
		.size = size,
	};
	struct io_parameter param = {
		.op = P2P_IO_READ,
		.file_name = source,
		.file_offset = 0,
		.iov = &iov,
		.iov_nr = 1,
	};

	return rw_file(io_fd, &param);
}

static int case_recycle_idle(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int before;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	before = stub_notify_cb_calls();
	if (before < 0) {
		err = before;
		goto out;
	}
	err = stub_notify_free(REG_ADDR, REG_SIZE);
	if (err)
		goto out;
	err = expect("recycle idle callback", stub_notify_cb_calls() > before, 1);
	if (err)
		goto out;
	err = expect_one_of("submit after hbm recycle",
			    submit_one(fds.io, topology, handle, REG_ADDR, IO_SIZE),
			    -ENOENT, -ESTALE);
	if (err)
		goto out;
	err = expect("unregister after hbm recycle",
		     unregister_handle(fds.owner, handle), -ENOENT);
	if (err)
		goto out;
	err = drain_io(fds.io);

out:
	fds_close(&fds);
	return err;
}

static int case_recycle_inflight(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int before;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	before = stub_notify_cb_calls();
	if (before < 0) {
		err = before;
		goto out;
	}
	err = stub_notify_free(REG_ADDR, REG_SIZE);
	if (err)
		goto out;
	err = expect("recycle inflight callback", stub_notify_cb_calls() > before, 1);
	if (err)
		goto out;
	err = expect_one_of("submit after inflight recycle",
			    submit_one(fds.io, topology, handle, REG_ADDR, IO_SIZE),
			    -ENOENT, -ESTALE);
	if (err)
		goto out;
	err = drain_io(fds.io);
	if (err)
		goto out;
	err = expect("unregister after inflight recycle",
		     unregister_handle(fds.owner, handle), -ENOENT);

out:
	fds_close(&fds);
	return err;
}

static int case_recycle_oneshot(const char *topology)
{
	struct fds fds;
	int before;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = submit_oneshot(fds.io, topology, REG_ADDR, IO_SIZE);
	if (err)
		goto out;
	before = stub_notify_cb_calls();
	if (before < 0) {
		err = before;
		goto out;
	}
	err = stub_notify_free(REG_ADDR, IO_SIZE);
	if (err)
		goto out;
	err = expect("oneshot recycle callback", stub_notify_cb_calls() > before, 1);
	if (err)
		goto out;
	err = drain_io(fds.io);

out:
	fds_close(&fds);
	return err;
}

static int case_recycle_then_close(const char *topology)
{
	struct fds fds;
	uint64_t handle;
	int err;

	fds_init(&fds);
	err = fds_open_pair(&fds, topology);
	if (err)
		goto out;
	err = register_range(fds.owner, REG_ADDR, REG_SIZE, &handle);
	if (err)
		goto out;
	err = submit_reads(fds.io, topology, handle, IO_COUNT);
	if (err)
		goto out;
	err = stub_notify_free(REG_ADDR, REG_SIZE);
	if (err)
		goto out;
	err = close_p2p_fd(fds.owner);
	fds.owner = -1;
	if (err)
		goto out;
	err = drain_io(fds.io);

out:
	fds_close(&fds);
	return err;
}

static const struct named_case kcases[] = {
	{ "close-owner-inflight", case_close_owner_inflight },
	{ "unregister-inflight", case_unregister_inflight },
	{ "close-io-first", case_close_io_first },
	{ "idle-close", case_idle_close },
	{ "multi-mem-revoke", case_multi_mem_revoke },
	{ "reregister-after-close", case_reregister_after_close },
	{ "many-io-fds", case_many_io_fds },
	{ "close-races-submit", case_close_races_submit },
	{ "recycle-idle", case_recycle_idle },
	{ "recycle-inflight", case_recycle_inflight },
	{ "recycle-oneshot", case_recycle_oneshot },
	{ "recycle-then-close", case_recycle_then_close },
};

static void usage(const char *program)
{
	unsigned int i;

	fprintf(stderr,
		"Usage: %s --topology <block-device> [--case NAME]\n"
		"Cases:",
		program);
	for (i = 0; i < sizeof(kcases) / sizeof(kcases[0]); i++)
		fprintf(stderr, " %s", kcases[i].name);
	fprintf(stderr, " exit-inflight kill-inflight all\n");
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "case", required_argument, NULL, 'c' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topology = NULL;
	const char *case_name = "all";
	unsigned int i;
	bool ran = false;
	int option;
	int err = 0;

	while ((option = getopt_long(argc, argv, "t:c:", options, NULL)) != -1) {
		switch (option) {
		case 't':
			topology = optarg;
			break;
		case 'c':
			case_name = optarg;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (!topology || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	for (i = 0; i < sizeof(kcases) / sizeof(kcases[0]); i++) {
		if (strcmp(case_name, "all") && strcmp(case_name, kcases[i].name))
			continue;
		printf("running %s\n", kcases[i].name);
		err = kcases[i].run(topology);
		if (err) {
			fprintf(stderr, "%s failed: %s (%d)\n", kcases[i].name,
				strerror(-err), err);
			return EXIT_FAILURE;
		}
		ran = true;
	}

	if (!strcmp(case_name, "all") || !strcmp(case_name, "exit-inflight")) {
		printf("running exit-inflight\n");
		err = run_child_exit_or_kill(topology, false);
		if (err) {
			fprintf(stderr, "exit-inflight failed: %s (%d)\n",
				strerror(-err), err);
			return EXIT_FAILURE;
		}
		ran = true;
	}
	if (!strcmp(case_name, "all") || !strcmp(case_name, "kill-inflight")) {
		printf("running kill-inflight\n");
		err = run_child_exit_or_kill(topology, true);
		if (err) {
			fprintf(stderr, "kill-inflight failed: %s (%d)\n",
				strerror(-err), err);
			return EXIT_FAILURE;
		}
		ran = true;
	}

	if (!ran) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	printf("registered-memory lifetime tests passed\n");
	return EXIT_SUCCESS;
}
