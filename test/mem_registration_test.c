// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "file_p2p_api.h"

#define TEST_REG_ADDR 512UL
#define TEST_REG_SIZE ((4UL << 20) - TEST_REG_ADDR)
#define TEST_SECOND_REG_ADDR ((8UL << 20) + 512)
#define TEST_SECOND_REG_SIZE ((4UL << 20) - 512)
#define TEST_RACE_THREADS 8
#define TEST_UNREGISTER_TIMEOUT_SEC 30

struct race_context {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool go;
	const char *source;
	uint64_t handle;
	int dev_fd;
};

struct race_worker {
	struct race_context *context;
	unsigned int index;
	int result;
};

struct unregister_worker {
	uint64_t handle;
	int dev_fd;
	int result;
};

static int expect(const char *operation, int actual, int expected)
{
	printf("api=c case=%s ret=%d expected=%d\n", operation, actual, expected);
	if (actual == expected)
		return 0;
	fprintf(stderr, "%s returned %d, expected %d\n", operation, actual,
		expected);
	return -EINVAL;
}

static int submit_read(int dev_fd, const char *source,
		       struct p2p_iov *iov, unsigned int iov_nr,
		       unsigned int flags, uint64_t mem_handle)
{
	struct io_parameter param = {
		.op = P2P_IO_READ,
		.file_name = source,
		.file_offset = 0,
		.iov = iov,
		.iov_nr = iov_nr,
		.flags = flags,
		.mem_handle = mem_handle,
	};

	return rw_file(dev_fd, &param);
}

static void *race_submit(void *argument)
{
	struct race_worker *worker = argument;
	struct race_context *context = worker->context;
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR + (worker->index + 32) * 512,
		.size = 512,
	};

	pthread_mutex_lock(&context->lock);
	while (!context->go)
		pthread_cond_wait(&context->cond, &context->lock);
	pthread_mutex_unlock(&context->lock);

	worker->result = submit_read(context->dev_fd, context->source, &iov, 1,
				     P2P_IO_F_REGISTERED_MEM,
				     context->handle);
	return NULL;
}

static int register_range_at(int dev_fd, unsigned long addr,
			     unsigned long size, uint64_t *handle)
{
	struct p2p_mem_register_param param = {
		.addr = addr,
		.size = size,
	};
	int err;

	err = register_mem(dev_fd, &param);
	if (err)
		return err;
	if (!param.mem_handle)
		return -EINVAL;
	*handle = param.mem_handle;
	return 0;
}

static int register_range(int dev_fd, uint64_t *handle)
{
	return register_range_at(dev_fd, TEST_REG_ADDR, TEST_REG_SIZE, handle);
}

static int unregister_handle(int dev_fd, uint64_t handle)
{
	struct p2p_mem_unregister_param param = {
		.mem_handle = handle,
	};

	return unregister_mem(dev_fd, &param);
}

static void *race_unregister(void *argument)
{
	struct unregister_worker *worker = argument;

	worker->result = unregister_handle(worker->dev_fd, worker->handle);
	return NULL;
}

static int timed_join(pthread_t thread, unsigned int timeout_sec)
{
	struct timespec deadline;
	int err;

	if (clock_gettime(CLOCK_REALTIME, &deadline))
		return -errno;
	deadline.tv_sec += timeout_sec;
	err = pthread_timedjoin_np(thread, NULL, &deadline);
	return err ? -err : 0;
}

static int test_validation(int dev_fd, int other_fd, const char *source,
			   uint64_t handle)
{
	struct p2p_mem_register_param bad_register = {
		.addr = TEST_REG_ADDR,
		.size = TEST_REG_SIZE,
		.reserved = 1,
	};
	struct p2p_mem_register_param zero_register = {
		.addr = TEST_REG_ADDR,
	};
	struct p2p_mem_register_param overflow_register = {
		.addr = UINT64_MAX,
		.size = 2,
	};
	struct p2p_mem_unregister_param bad_unregister = {
		.reserved = 1,
		.mem_handle = handle,
	};
	struct p2p_iov valid = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};
	struct p2p_iov outside = {
		.addr = TEST_REG_ADDR + TEST_REG_SIZE,
		.size = 512,
	};
	int err;

	err = expect("register reserved", register_mem(dev_fd, &bad_register),
		     -EINVAL);
	if (err)
		return err;
	err = expect("register zero size", register_mem(dev_fd, &zero_register),
		     -EINVAL);
	if (err)
		return err;
	err = expect("register overflow",
		     register_mem(dev_fd, &overflow_register), -EOVERFLOW);
	if (err)
		return err;
	err = expect("unregister reserved",
		     unregister_mem(dev_fd, &bad_unregister), -EINVAL);
	if (err)
		return err;
	err = expect("unregister zero handle", unregister_handle(dev_fd, 0),
		     -EINVAL);
	if (err)
		return err;
	err = expect("zero registered handle",
		     submit_read(dev_fd, source, &valid, 1,
				 P2P_IO_F_REGISTERED_MEM, 0), -EINVAL);
	if (err)
		return err;
	err = expect("unknown read flag",
		     submit_read(dev_fd, source, &valid, 1, 1UL << 1, handle),
		     -EOPNOTSUPP);
	if (err)
		return err;
	err = expect("registered range bounds",
		     submit_read(dev_fd, source, &outside, 1,
				 P2P_IO_F_REGISTERED_MEM, handle), -ERANGE);
	if (err)
		return err;
	err = expect("handle on another fd",
		     submit_read(other_fd, source, &valid, 1,
				 P2P_IO_F_REGISTERED_MEM, handle), 0);
	if (err)
		return err;
	err = drain_io(other_fd);
	if (err)
		return err;
	err = expect("unregister on another fd",
		     unregister_handle(other_fd, handle), -EPERM);
	if (err)
		return err;

	return 0;
}

static int test_process_scope(int dev_fd, const char *source, uint64_t handle)
{
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return -errno;
	if (!child) {
		int read_err;
		int unregister_err;

		read_err = submit_read(dev_fd, source, &iov, 1,
				       P2P_IO_F_REGISTERED_MEM, handle);
		unregister_err = unregister_handle(dev_fd, handle);
		_exit(read_err == -EPERM && unregister_err == -EPERM ?
		      EXIT_SUCCESS : EXIT_FAILURE);
	}
	if (waitpid(child, &status, 0) < 0)
		return -errno;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
		fprintf(stderr, "inherited-fd process-scope check failed\n");
		return -EINVAL;
	}
	return 0;
}

static int test_multiple_registered_mem(int owner_fd, int io_fd,
					const char *source,
					uint64_t primary_handle)
{
	struct p2p_iov primary_iov = {
		.addr = TEST_REG_ADDR + 512,
		.size = 512,
	};
	struct p2p_iov second_iov = {
		.addr = TEST_SECOND_REG_ADDR,
		.size = 512,
	};
	uint64_t second_handle = 0;
	uint64_t stale_handle;
	int err;

	err = register_range_at(owner_fd, TEST_SECOND_REG_ADDR,
				TEST_SECOND_REG_SIZE, &second_handle);
	if (err)
		return err;
	if (second_handle == primary_handle) {
		err = -EINVAL;
		goto out;
	}
	err = submit_read(io_fd, source, &primary_iov, 1,
			  P2P_IO_F_REGISTERED_MEM, primary_handle);
	if (err)
		goto out;
	err = submit_read(io_fd, source, &second_iov, 1,
			  P2P_IO_F_REGISTERED_MEM, second_handle);
	if (err)
		goto out;
	err = drain_io(io_fd);
	if (err)
		goto out;
	err = unregister_handle(owner_fd, second_handle);
	if (err)
		goto out;
	stale_handle = second_handle;
	second_handle = 0;
	err = expect("unregistered second handle",
		     submit_read(io_fd, source, &second_iov, 1,
				 P2P_IO_F_REGISTERED_MEM, stale_handle), -ENOENT);
	if (err)
		goto out;
	err = submit_read(io_fd, source, &primary_iov, 1,
			  P2P_IO_F_REGISTERED_MEM, primary_handle);
	if (err)
		goto out;

out:
	{
		int drain_err = drain_io(io_fd);

		if (!err)
			err = drain_err;
	}
	if (second_handle)
		unregister_handle(owner_fd, second_handle);
	return err;
}

static int test_normal_handle_ignored(int dev_fd, const char *source)
{
	struct p2p_iov iov = {
		.addr = 8UL << 20,
		.size = 512,
	};
	int err;

	err = submit_read(dev_fd, source, &iov, 1, 0, UINT64_MAX);
	if (err)
		return err;
	return drain_io(dev_fd);
}

static int test_unregister_before_drain(int owner_fd, int io_fd,
					const char *source, uint64_t handle)
{
	struct p2p_iov multi_iov[] = {
		{ .addr = TEST_REG_ADDR, .size = 512 },
		{ .addr = (2UL << 20) - 512, .size = 1024 },
		{ .addr = (2UL << 20) + 512, .size = 1536 },
	};
	struct race_context context = {
		.source = source,
		.handle = handle,
		.dev_fd = io_fd,
	};
	struct race_worker workers[TEST_RACE_THREADS];
	struct unregister_worker unregister = {
		.handle = handle,
		.dev_fd = owner_fd,
		.result = -EINPROGRESS,
	};
	pthread_t threads[TEST_RACE_THREADS];
	pthread_t unregister_thread;
	bool unregister_started = false;
	unsigned int started = 0;
	unsigned int i;
	int err;

	err = submit_read(io_fd, source, multi_iov,
			  sizeof(multi_iov) / sizeof(multi_iov[0]),
			  P2P_IO_F_REGISTERED_MEM, handle);
	if (err)
		return err;

	for (i = 0; i < 16; i++) {
		struct p2p_iov iov = {
			.addr = TEST_REG_ADDR + (unsigned long)i * 512,
			.size = 512,
		};

		err = submit_read(io_fd, source, &iov, 1,
				  P2P_IO_F_REGISTERED_MEM, handle);
		if (err)
			return err;
	}

	if (pthread_mutex_init(&context.lock, NULL))
		return -ENOMEM;
	if (pthread_cond_init(&context.cond, NULL)) {
		pthread_mutex_destroy(&context.lock);
		return -ENOMEM;
	}
	for (i = 0; i < TEST_RACE_THREADS; i++) {
		workers[i].context = &context;
		workers[i].index = i;
		workers[i].result = -EINPROGRESS;
		if (pthread_create(&threads[i], NULL, race_submit, &workers[i])) {
			err = -ENOMEM;
			goto release_workers;
		}
		started++;
	}

	pthread_mutex_lock(&context.lock);
	context.go = true;
	pthread_cond_broadcast(&context.cond);
	pthread_mutex_unlock(&context.lock);
	if (pthread_create(&unregister_thread, NULL, race_unregister, &unregister)) {
		err = -ENOMEM;
		goto release_workers;
	}
	unregister_started = true;

release_workers:
	pthread_mutex_lock(&context.lock);
	context.go = true;
	pthread_cond_broadcast(&context.cond);
	pthread_mutex_unlock(&context.lock);
	for (i = 0; i < started; i++) {
		pthread_join(threads[i], NULL);
		if (!workers[i].result || workers[i].result == -ENOENT ||
		    workers[i].result == -ESTALE)
			continue;
		fprintf(stderr, "racing read %u returned %d\n", i,
			workers[i].result);
		if (!err)
			err = workers[i].result;
	}
	if (unregister_started) {
		int join_err;

		/* Unregister must depend on hardware completion, not io_fd drain. */
		join_err = timed_join(unregister_thread,
				      TEST_UNREGISTER_TIMEOUT_SEC);
		if (join_err) {
			fprintf(stderr,
				"unregister did not finish before drain: %s\n",
				strerror(-join_err));
			if (!err)
				err = join_err;
		} else {
			unregister_started = false;
			if (!err)
				err = unregister.result;
		}
	}
	{
		int drain_err = drain_io(io_fd);

		if (!err)
			err = drain_err;
	}
	if (unregister_started) {
		pthread_join(unregister_thread, NULL);
		if (!err)
			err = unregister.result;
	}
	pthread_cond_destroy(&context.cond);
	pthread_mutex_destroy(&context.lock);
	return err;
}

static int test_handle_sequence(int dev_fd, const char *source, uint64_t old_handle)
{
	struct p2p_iov iov = {
		.addr = TEST_REG_ADDR,
		.size = 512,
	};
	uint64_t new_handle;
	int err;

	err = register_range(dev_fd, &new_handle);
	if (err)
		return err;
	if (old_handle == new_handle) {
		fprintf(stderr, "registered memory handle was reused: old=0x%llx new=0x%llx\n",
			(unsigned long long)old_handle,
			(unsigned long long)new_handle);
		err = -EINVAL;
		goto unregister_new;
	}
	err = expect("stale registered handle",
		     submit_read(dev_fd, source, &iov, 1,
				 P2P_IO_F_REGISTERED_MEM, old_handle), -ENOENT);
	if (err)
		goto unregister_new;
	err = expect("stale unregister", unregister_handle(dev_fd, old_handle),
		     -ENOENT);

unregister_new:
	if (unregister_handle(dev_fd, new_handle) && !err)
		err = -EINVAL;
	return err;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s --topology <block-device>\n", program);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ NULL, 0, NULL, 0 },
	};
	const char *topology = NULL;
	uint64_t handle = 0;
	uint64_t close_handle;
	int other_fd = -1;
	int close_fd = -1;
	int dev_fd = -1;
	int option;
	int err;

	while ((option = getopt_long(argc, argv, "t:", options, NULL)) != -1) {
		if (option == 't') {
			topology = optarg;
		} else {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (!topology || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	dev_fd = new_p2p_fd();
	other_fd = new_p2p_fd();
	if (dev_fd < 0 || other_fd < 0) {
		err = dev_fd < 0 ? dev_fd : other_fd;
		goto out;
	}
	err = expect("add topology owner", add_topo(dev_fd, topology), 0);
	if (err)
		goto out;
	err = expect("add topology io", add_topo(other_fd, topology), 0);
	if (err)
		goto out;
	err = expect("register primary", register_range(dev_fd, &handle), 0);
	if (err)
		goto out;
	err = test_validation(dev_fd, other_fd, topology, handle);
	if (err)
		goto unregister;
	err = expect("multiple registered memories",
		     test_multiple_registered_mem(dev_fd, other_fd, topology,
						  handle), 0);
	if (err)
		goto unregister;
	err = expect("process scope", test_process_scope(dev_fd, topology, handle),
		     0);
	if (err)
		goto unregister;
	err = expect("normal handle ignored",
		     test_normal_handle_ignored(dev_fd, topology), 0);
	if (err)
		goto unregister;
	err = expect("unregister before drain",
		     test_unregister_before_drain(dev_fd, other_fd, topology,
					  handle), 0);
	if (err)
		goto out;
	err = expect("handle sequence",
		     test_handle_sequence(dev_fd, topology, handle), 0);
	if (err)
		goto out;

	close_fd = new_p2p_fd();
	if (close_fd < 0) {
		err = close_fd;
		goto out;
	}
	err = expect("register close cleanup",
		     register_range(close_fd, &close_handle), 0);
	if (err)
		goto out;
	close_p2p_fd(close_fd);
	close_fd = -1;

	printf("memory registration tests passed\n");
	err = 0;
	goto out;

unregister:
	unregister_handle(dev_fd, handle);
out:
	if (close_fd >= 0)
		close_p2p_fd(close_fd);
	if (other_fd >= 0)
		close_p2p_fd(other_fd);
	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	if (err) {
		fprintf(stderr, "memory registration test failed: %d\n", err);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
