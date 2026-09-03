/* SPDX-License-Identifier: GPL-2.0 */
#ifndef P2P_DEBUGFS_TEST_H_
#define P2P_DEBUGFS_TEST_H_

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct debugfs_reader_context {
	const char *path;
	pthread_barrier_t start;
	pthread_t thread;
	atomic_bool stop;
	int result;
};

static int debugfs_read_file(const char *path, const char *expected)
{
	char line[256];
	bool found = !expected;
	FILE *file;
	int err = 0;

	file = fopen(path, "r");
	if (!file)
		return -errno;
	while (fgets(line, sizeof(line), file)) {
		if (expected && strstr(line, expected))
			found = true;
	}
	if (ferror(file))
		err = -EIO;
	else if (!found)
		err = -ENOENT;
	fclose(file);
	return err;
}

static void *debugfs_reader_run(void *argument)
{
	struct debugfs_reader_context *context = argument;

	context->result = debugfs_read_file(context->path, NULL);
	pthread_barrier_wait(&context->start);
	if (!context->result) {
		do {
			context->result = debugfs_read_file(context->path, NULL);
		} while (!context->result &&
			 !atomic_load_explicit(&context->stop, memory_order_relaxed));
	}
	return NULL;
}

static int debugfs_reader_start(struct debugfs_reader_context *context, const char *path)
{
	int err;

	context->path = path;
	context->result = 0;
	atomic_init(&context->stop, false);
	err = pthread_barrier_init(&context->start, NULL, 2);
	if (err)
		return -err;
	err = pthread_create(&context->thread, NULL, debugfs_reader_run, context);
	if (err) {
		pthread_barrier_destroy(&context->start);
		return -err;
	}
	pthread_barrier_wait(&context->start);
	return 0;
}

static int debugfs_reader_stop(struct debugfs_reader_context *context)
{
	int err;

	atomic_store_explicit(&context->stop, true, memory_order_relaxed);
	err = pthread_join(context->thread, NULL);
	pthread_barrier_destroy(&context->start);
	if (err)
		return -err;
	return context->result;
}

#endif
