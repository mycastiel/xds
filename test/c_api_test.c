#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_p2p_api.h"

#define STRESS_KIB (1UL << 10)
#define STRESS_MIN_LENGTH (4UL << 10)
#define STRESS_MAX_LENGTH (4UL << 20)

enum run_mode {
	MODE_SINGLE,
	MODE_QUEUED,
	MODE_THREADED,
	MODE_STRESS,
};

struct test_case {
	char *id;
	char *path;
	unsigned long file_offset;
	unsigned long cmb_va;
	unsigned long length;
	int expected;
	int result;
};

struct start_gate {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool go;
	bool abort;
};

struct worker {
	struct start_gate *gate;
	struct test_case *test;
	int dev_fd;
};

struct stress_case {
	unsigned int iteration;
	unsigned int worker;
	char *id;
	char *path;
	unsigned long file_size;
	unsigned long file_offset;
	unsigned long length;
	unsigned long cmb_va;
	int result;
};

struct va_allocator {
	pthread_mutex_t lock;
	unsigned char *bitmap;
	size_t slots;
	size_t cursor;
	unsigned long granularity;
};

struct stress_context;

struct stress_worker {
	struct stress_context *context;
	unsigned int worker;
};

struct stress_context {
	const char *topology;
	struct stress_case **matrix;
	struct va_allocator allocator;
	struct start_gate gate;
	pthread_barrier_t phase;
	pthread_mutex_t error_lock;
	int *fds;
	unsigned int workers;
	unsigned int iterations;
	unsigned long cmb_size;
	unsigned long granularity;
	unsigned long backing_page_size;
	int error;
};

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --topology <block-device> --manifest <tsv> "
		"--mode <single|queued|threaded|stress> "
		"[stress options]\n",
		program);
}

static int parse_ulong(const char *text, unsigned long *value)
{
	unsigned long parsed;
	char *end;

	if (!text || !*text || *text == '-')
		return -EINVAL;
	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno == ERANGE)
		return -ERANGE;
	if (end == text || *end)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static int parse_int(const char *text, int *value)
{
	long parsed;
	char *end;

	if (!text || !*text)
		return -EINVAL;
	errno = 0;
	parsed = strtol(text, &end, 0);
	if (errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
		return -ERANGE;
	if (end == text || *end)
		return -EINVAL;
	*value = (int)parsed;
	return 0;
}

static int parse_uint(const char *text, unsigned int *value)
{
	unsigned long parsed;
	int err;

	err = parse_ulong(text, &parsed);
	if (err)
		return err;
	if (parsed > UINT_MAX)
		return -ERANGE;
	*value = (unsigned int)parsed;
	return 0;
}

static void free_cases(struct test_case *tests, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		free(tests[i].id);
		free(tests[i].path);
	}
	free(tests);
}

static int append_case(struct test_case **tests, size_t *count,
		       size_t *capacity, char *fields[6], unsigned int line_nr)
{
	struct test_case *test;
	struct test_case *new_tests;
	size_t new_capacity;
	int err;

	if (*count == *capacity) {
		new_capacity = *capacity ? *capacity * 2 : 8;
		if (new_capacity > SIZE_MAX / sizeof(**tests))
			return -E2BIG;
		new_tests = realloc(*tests, new_capacity * sizeof(**tests));
		if (!new_tests)
			return -ENOMEM;
		*tests = new_tests;
		*capacity = new_capacity;
	}

	test = &(*tests)[*count];
	memset(test, 0, sizeof(*test));
	test->id = strdup(fields[0]);
	test->path = strdup(fields[1]);
	if (!test->id || !test->path) {
		free(test->id);
		free(test->path);
		return -ENOMEM;
	}
	err = parse_ulong(fields[2], &test->file_offset);
	if (!err)
		err = parse_ulong(fields[3], &test->cmb_va);
	if (!err)
		err = parse_ulong(fields[4], &test->length);
	if (!err)
		err = parse_int(fields[5], &test->expected);
	if (err) {
		fprintf(stderr, "invalid manifest value at line %u\n", line_nr);
		free(test->id);
		free(test->path);
		return err;
	}
	if (!*test->id || !*test->path || !test->length) {
		fprintf(stderr, "empty manifest field at line %u\n", line_nr);
		free(test->id);
		free(test->path);
		return -EINVAL;
	}

	(*count)++;
	return 0;
}

static int load_manifest(const char *path, struct test_case **tests_out,
			 size_t *count_out)
{
	struct test_case *tests = NULL;
	size_t capacity = 0;
	size_t count = 0;
	size_t line_size = 0;
	unsigned int line_nr = 0;
	char *line = NULL;
	FILE *file;
	int err = 0;

	file = fopen(path, "re");
	if (!file)
		return -errno;

	while (getline(&line, &line_size, file) >= 0) {
		char *fields[6];
		char *cursor;
		size_t len;
		unsigned int i;

		line_nr++;
		len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len || line[0] == '#')
			continue;

		cursor = line;
		for (i = 0; i < 6; i++) {
			fields[i] = strsep(&cursor, "\t");
			if (!fields[i])
				break;
		}
		if (i != 6 || cursor) {
			fprintf(stderr,
				"manifest line %u must have six TSV fields\n",
				line_nr);
			err = -EINVAL;
			break;
		}
		err = append_case(&tests, &count, &capacity, fields, line_nr);
		if (err)
			break;
	}
	if (!err && ferror(file))
		err = -errno;
	if (!err && !count)
		err = -EINVAL;

	free(line);
	fclose(file);
	if (err) {
		free_cases(tests, count);
		return err;
	}

	*tests_out = tests;
	*count_out = count;
	return 0;
}

static void free_stress_cases(struct stress_case *tests, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		free(tests[i].id);
		free(tests[i].path);
	}
	free(tests);
}

static int append_stress_case(struct stress_case **tests, size_t *count,
			      size_t *capacity, char *fields[7],
			      unsigned int line_nr)
{
	struct stress_case *test;
	struct stress_case *new_tests;
	size_t new_capacity;
	int err;

	if (*count == *capacity) {
		new_capacity = *capacity ? *capacity * 2 : 32;
		if (new_capacity > SIZE_MAX / sizeof(**tests))
			return -E2BIG;
		new_tests = realloc(*tests, new_capacity * sizeof(**tests));
		if (!new_tests)
			return -ENOMEM;
		*tests = new_tests;
		*capacity = new_capacity;
	}

	test = &(*tests)[*count];
	memset(test, 0, sizeof(*test));
	test->cmb_va = ULONG_MAX;
	test->result = INT_MIN;
	err = parse_uint(fields[0], &test->iteration);
	if (!err)
		err = parse_uint(fields[1], &test->worker);
	test->id = strdup(fields[2]);
	test->path = strdup(fields[3]);
	if (!test->id || !test->path) {
		free(test->id);
		free(test->path);
		return -ENOMEM;
	}
	if (!err)
		err = parse_ulong(fields[4], &test->file_size);
	if (!err)
		err = parse_ulong(fields[5], &test->file_offset);
	if (!err)
		err = parse_ulong(fields[6], &test->length);
	if (err || !*test->id || !*test->path || !test->file_size ||
	    !test->length) {
		fprintf(stderr, "invalid stress manifest entry at line %u\n",
			line_nr);
		free(test->id);
		free(test->path);
		return err ? err : -EINVAL;
	}

	(*count)++;
	return 0;
}

static int load_stress_manifest(const char *path,
				struct stress_case **tests_out,
				size_t *count_out)
{
	struct stress_case *tests = NULL;
	size_t capacity = 0;
	size_t count = 0;
	size_t line_size = 0;
	unsigned int line_nr = 0;
	char *line = NULL;
	FILE *file;
	int err = 0;

	file = fopen(path, "re");
	if (!file)
		return -errno;

	while (getline(&line, &line_size, file) >= 0) {
		char *fields[7];
		char *cursor;
		size_t len;
		unsigned int i;

		line_nr++;
		len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len || line[0] == '#')
			continue;

		cursor = line;
		for (i = 0; i < 7; i++) {
			fields[i] = strsep(&cursor, "\t");
			if (!fields[i])
				break;
		}
		if (i != 7 || cursor) {
			fprintf(stderr,
				"stress manifest line %u must have seven TSV fields\n",
				line_nr);
			err = -EINVAL;
			break;
		}
		err = append_stress_case(&tests, &count, &capacity, fields,
					 line_nr);
		if (err)
			break;
	}
	if (!err && ferror(file))
		err = -errno;
	if (!err && !count)
		err = -EINVAL;

	free(line);
	fclose(file);
	if (err) {
		free_stress_cases(tests, count);
		return err;
	}
	*tests_out = tests;
	*count_out = count;
	return 0;
}

static int submit_case(int dev_fd, struct test_case *test)
{
	struct p2p_iov iov = {
		.addr = test->cmb_va,
		.size = test->length,
	};
	struct read_parameter param = {
		.file_name = test->path,
		.file_offset = test->file_offset,
		.iov = &iov,
		.iov_nr = 1,
	};

	test->result = read_file(dev_fd, &param);
	printf("api=c case=%s ret=%d expected=%d\n", test->id, test->result,
	       test->expected);
	return test->result == test->expected ? 0 : -EINVAL;
}

static void *thread_worker(void *argument)
{
	struct worker *worker = argument;

	pthread_mutex_lock(&worker->gate->lock);
	while (!worker->gate->go)
		pthread_cond_wait(&worker->gate->cond, &worker->gate->lock);
	pthread_mutex_unlock(&worker->gate->lock);

	submit_case(worker->dev_fd, worker->test);
	return NULL;
}

static int run_single(int dev_fd, struct test_case *tests, size_t count)
{
	size_t i;
	int err;

	for (i = 0; i < count; i++) {
		err = submit_case(dev_fd, &tests[i]);
		if (err)
			return err;
		if (!tests[i].expected) {
			err = drain_io(dev_fd);
			if (err) {
				fprintf(stderr, "drain after %s failed: %d\n",
					tests[i].id, err);
				return err;
			}
		}
	}
	return drain_io(dev_fd);
}

static int run_queued(int dev_fd, struct test_case *tests, size_t count)
{
	size_t i;
	int err;

	for (i = 0; i < count; i++) {
		if (tests[i].expected) {
			fprintf(stderr,
				"queued mode only accepts successful cases\n");
			return -EINVAL;
		}
		err = submit_case(dev_fd, &tests[i]);
		if (err)
			return err;
	}
	return drain_io(dev_fd);
}

static int run_threaded(int dev_fd, struct test_case *tests, size_t count)
{
	struct start_gate gate = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
	};
	struct worker *workers;
	pthread_t *threads;
	size_t created = 0;
	size_t i;
	int err = 0;

	for (i = 0; i < count; i++) {
		if (tests[i].expected) {
			fprintf(stderr,
				"threaded mode only accepts successful cases\n");
			return -EINVAL;
		}
	}
	workers = calloc(count, sizeof(*workers));
	threads = calloc(count, sizeof(*threads));
	if (!workers || !threads) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < count; i++) {
		workers[i].gate = &gate;
		workers[i].test = &tests[i];
		workers[i].dev_fd = dev_fd;
		err = pthread_create(&threads[i], NULL, thread_worker,
				     &workers[i]);
		if (err) {
			err = -err;
			break;
		}
		created++;
	}
	pthread_mutex_lock(&gate.lock);
	gate.go = true;
	pthread_cond_broadcast(&gate.cond);
	pthread_mutex_unlock(&gate.lock);
	for (i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != count)
		goto out;
	for (i = 0; i < count; i++) {
		if (tests[i].result != tests[i].expected) {
			err = -EINVAL;
			goto out;
		}
	}
	err = drain_io(dev_fd);

out:
	free(threads);
	free(workers);
	pthread_cond_destroy(&gate.cond);
	pthread_mutex_destroy(&gate.lock);
	return err;
}

static int va_allocator_init(struct va_allocator *allocator,
			     unsigned long size, unsigned long granularity)
{
	if (!size || !granularity || size % granularity)
		return -EINVAL;
	allocator->slots = size / granularity;
	allocator->granularity = granularity;
	allocator->bitmap = calloc(allocator->slots, 1);
	if (!allocator->bitmap)
		return -ENOMEM;
	if (pthread_mutex_init(&allocator->lock, NULL)) {
		free(allocator->bitmap);
		allocator->bitmap = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void va_allocator_destroy(struct va_allocator *allocator)
{
	if (!allocator->bitmap)
		return;
	pthread_mutex_destroy(&allocator->lock);
	free(allocator->bitmap);
	allocator->bitmap = NULL;
}

static int va_allocate(struct va_allocator *allocator, unsigned long length,
		       unsigned long *va)
{
	size_t needed;
	size_t scanned;
	int err = -ENOSPC;

	if (length > ULONG_MAX - allocator->granularity + 1)
		return -EOVERFLOW;
	needed = (length + allocator->granularity - 1) /
		 allocator->granularity;
	if (!needed || needed > allocator->slots)
		return -ENOSPC;

	pthread_mutex_lock(&allocator->lock);
	for (scanned = 0; scanned < allocator->slots; scanned++) {
		size_t start = (allocator->cursor + scanned) % allocator->slots;
		size_t i;

		if (start + needed > allocator->slots)
			continue;
		for (i = 0; i < needed; i++) {
			if (allocator->bitmap[start + i])
				break;
		}
		if (i != needed)
			continue;
		memset(&allocator->bitmap[start], 1, needed);
		allocator->cursor = (start + needed) % allocator->slots;
		*va = start * allocator->granularity;
		err = 0;
		break;
	}
	pthread_mutex_unlock(&allocator->lock);
	return err;
}

static int va_release(struct va_allocator *allocator, unsigned long va,
		      unsigned long length)
{
	size_t start;
	size_t needed;
	size_t i;
	int err = 0;

	if (va % allocator->granularity ||
	    length > ULONG_MAX - allocator->granularity + 1)
		return -EINVAL;
	start = va / allocator->granularity;
	needed = (length + allocator->granularity - 1) /
		 allocator->granularity;
	if (!needed || start > allocator->slots ||
	    needed > allocator->slots - start)
		return -EINVAL;

	pthread_mutex_lock(&allocator->lock);
	for (i = 0; i < needed; i++) {
		if (!allocator->bitmap[start + i]) {
			err = -EINVAL;
			break;
		}
	}
	if (!err)
		memset(&allocator->bitmap[start], 0, needed);
	pthread_mutex_unlock(&allocator->lock);
	return err;
}

static bool stress_failed(struct stress_context *context)
{
	bool failed;

	pthread_mutex_lock(&context->error_lock);
	failed = context->error != 0;
	pthread_mutex_unlock(&context->error_lock);
	return failed;
}

static void stress_fail(struct stress_context *context, int err,
			const char *operation, unsigned int worker)
{
	if (!err)
		err = -EINVAL;
	pthread_mutex_lock(&context->error_lock);
	if (!context->error) {
		context->error = err;
		fprintf(stderr, "stress worker %u: %s failed: %d\n",
			worker, operation, err);
	}
	pthread_mutex_unlock(&context->error_lock);
}

static int validate_stress_round(struct stress_context *context,
				 unsigned int iteration)
{
	bool shared = false;
	unsigned int i;

	for (i = 0; i < context->workers; i++) {
		struct stress_case *test =
			context->matrix[iteration * context->workers + i];
		unsigned long reserved;
		unsigned int j;

		if (test->cmb_va == ULONG_MAX ||
		    test->length > ULONG_MAX - context->granularity + 1)
			return -EINVAL;
		reserved = (test->length + context->granularity - 1) /
			   context->granularity * context->granularity;
		if (test->cmb_va % context->granularity ||
		    test->cmb_va > context->cmb_size ||
		    reserved > context->cmb_size - test->cmb_va)
			return -ERANGE;

		for (j = i + 1; j < context->workers; j++) {
			struct stress_case *other =
				context->matrix[iteration * context->workers + j];
			unsigned long other_reserved;
			unsigned long test_last_page;
			unsigned long other_last_page;

			if (other->cmb_va == ULONG_MAX ||
			    other->length > ULONG_MAX - context->granularity + 1)
				return -EINVAL;
			other_reserved =
				(other->length + context->granularity - 1) /
				context->granularity * context->granularity;
			if (test->cmb_va < other->cmb_va + other_reserved &&
			    other->cmb_va < test->cmb_va + reserved)
				return -EEXIST;

			test_last_page = (test->cmb_va + test->length - 1) /
					 context->backing_page_size;
			other_last_page = (other->cmb_va + other->length - 1) /
					  context->backing_page_size;
			if (test->cmb_va / context->backing_page_size <=
			    other_last_page &&
			    other->cmb_va / context->backing_page_size <=
			    test_last_page)
				shared = true;
		}
	}
	return shared ? 0 : -ENODATA;
}

static int submit_stress_case(int dev_fd, struct stress_case *test)
{
	struct p2p_iov iov = {
		.addr = test->cmb_va,
		.size = test->length,
	};
	struct read_parameter param = {
		.file_name = test->path,
		.file_offset = test->file_offset,
		.iov = &iov,
		.iov_nr = 1,
	};
	int err;

	test->result = read_file(dev_fd, &param);
	printf("api=c case=%s va=0x%lx ret=%d expected=0\n", test->id,
	       test->cmb_va, test->result);
	if (test->result)
		return test->result;
	err = drain_io(dev_fd);
	if (err)
		fprintf(stderr, "%s: drain_io failed: %d\n", test->id, err);
	return err;
}

static void *stress_thread_worker(void *argument)
{
	struct stress_worker *worker = argument;
	struct stress_context *context = worker->context;
	unsigned int worker_id = worker->worker;
	int dev_fd = -1;
	unsigned int iteration;
	int err;

	pthread_mutex_lock(&context->gate.lock);
	while (!context->gate.go && !context->gate.abort)
		pthread_cond_wait(&context->gate.cond, &context->gate.lock);
	if (context->gate.abort) {
		pthread_mutex_unlock(&context->gate.lock);
		return NULL;
	}
	pthread_mutex_unlock(&context->gate.lock);

	dev_fd = new_p2p_fd();
	if (dev_fd < 0)
		stress_fail(context, dev_fd, "new_p2p_fd", worker_id);
	else {
		context->fds[worker_id] = dev_fd;
		err = add_topo(dev_fd, context->topology);
		if (err)
			stress_fail(context, err, "add_topo", worker_id);
	}
	pthread_barrier_wait(&context->phase);
	if (!worker_id && !stress_failed(context)) {
		unsigned int i;
		unsigned int j;

		for (i = 0; i < context->workers; i++) {
			if (context->fds[i] < 0) {
				stress_fail(context, -EBADF, "fd validation", i);
				break;
			}
			for (j = i + 1; j < context->workers; j++) {
				if (context->fds[i] == context->fds[j]) {
					stress_fail(context, -EEXIST,
						    "distinct fd validation", i);
					break;
				}
			}
		}
	}
	pthread_barrier_wait(&context->phase);

	for (iteration = 0; iteration < context->iterations; iteration++) {
		struct stress_case *test =
			context->matrix[iteration * context->workers + worker_id];
		unsigned long va = ULONG_MAX;

		if (!stress_failed(context)) {
			err = va_allocate(&context->allocator, test->length, &va);
			if (err)
				stress_fail(context, err, "VA allocation", worker_id);
			else
				test->cmb_va = va;
		}
		pthread_barrier_wait(&context->phase);
		if (!worker_id && !stress_failed(context)) {
			err = validate_stress_round(context, iteration);
			if (err)
				stress_fail(context, err, "allocation validation",
					    worker_id);
		}
		pthread_barrier_wait(&context->phase);

		if (!stress_failed(context) && dev_fd >= 0 && va != ULONG_MAX) {
			err = submit_stress_case(dev_fd, test);
			if (err)
				stress_fail(context, err, "read", worker_id);
		}
		pthread_barrier_wait(&context->phase);

		if (va != ULONG_MAX) {
			err = va_release(&context->allocator, va, test->length);
			if (err)
				stress_fail(context, err, "VA release", worker_id);
		}
		pthread_barrier_wait(&context->phase);
	}

	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	return NULL;
}

static int prepare_stress_matrix(struct stress_case *tests, size_t count,
				 unsigned int workers,
				 unsigned int iterations,
				 struct stress_case ***matrix_out)
{
	struct stress_case **matrix;
	size_t expected;
	size_t i;

	if (!workers || !iterations || workers > SIZE_MAX / iterations)
		return -EINVAL;
	expected = (size_t)workers * iterations;
	if (count != expected)
		return -EINVAL;
	matrix = calloc(expected, sizeof(*matrix));
	if (!matrix)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct stress_case *test = &tests[i];
		size_t index;

		if (test->iteration >= iterations || test->worker >= workers ||
		    test->file_size % STRESS_KIB ||
		    test->file_offset % STRESS_KIB ||
		    test->length % STRESS_KIB ||
		    test->length < STRESS_MIN_LENGTH ||
		    test->length > STRESS_MAX_LENGTH ||
		    test->file_offset > test->file_size ||
		    test->length > test->file_size - test->file_offset) {
			free(matrix);
			return -EINVAL;
		}
		index = (size_t)test->iteration * workers + test->worker;
		if (matrix[index]) {
			free(matrix);
			return -EEXIST;
		}
		matrix[index] = test;
	}
	for (i = 0; i < expected; i++) {
		if (!matrix[i]) {
			free(matrix);
			return -EINVAL;
		}
	}
	*matrix_out = matrix;
	return 0;
}

static int write_stress_results(const char *path, struct stress_case **matrix,
				unsigned int workers,
				unsigned int iterations)
{
	FILE *file;
	unsigned int iteration;

	file = fopen(path, "we");
	if (!file)
		return -errno;
	for (iteration = 0; iteration < iterations; iteration++) {
		unsigned int worker;

		for (worker = 0; worker < workers; worker++) {
			struct stress_case *test =
				matrix[iteration * workers + worker];

			if (test->cmb_va == ULONG_MAX || test->result) {
				fclose(file);
				return -EINVAL;
			}
			if (fprintf(file, "%s\t%s\t%lu\t%lu\t%lu\t0\n",
				    test->id, test->path, test->file_offset,
				    test->cmb_va, test->length) < 0) {
				fclose(file);
				return -EIO;
			}
		}
	}
	if (fclose(file))
		return -errno;
	return 0;
}

static int run_stress(const char *topology, struct stress_case *tests,
		      size_t count, unsigned int workers,
		      unsigned int iterations, unsigned long cmb_size,
		      unsigned long granularity,
		      unsigned long backing_page_size,
		      const char *result_manifest)
{
	struct stress_context context = {
		.topology = topology,
		.workers = workers,
		.iterations = iterations,
		.cmb_size = cmb_size,
		.granularity = granularity,
		.backing_page_size = backing_page_size,
		.gate = {
			.lock = PTHREAD_MUTEX_INITIALIZER,
			.cond = PTHREAD_COND_INITIALIZER,
		},
		.error_lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct stress_worker *worker_args = NULL;
	pthread_t *threads = NULL;
	unsigned int created = 0;
	unsigned int i;
	bool barrier_initialized = false;
	int err;

	if (workers < 2 || !iterations || !cmb_size || !granularity ||
	    !backing_page_size || cmb_size % granularity ||
	    backing_page_size % granularity)
		return -EINVAL;
	err = prepare_stress_matrix(tests, count, workers, iterations,
				    &context.matrix);
	if (err)
		goto out;
	err = va_allocator_init(&context.allocator, cmb_size, granularity);
	if (err)
		goto out;
	context.fds = malloc(workers * sizeof(*context.fds));
	worker_args = calloc(workers, sizeof(*worker_args));
	threads = calloc(workers, sizeof(*threads));
	if (!context.fds || !worker_args || !threads) {
		err = -ENOMEM;
		goto out;
	}
	for (i = 0; i < workers; i++)
		context.fds[i] = -1;
	err = pthread_barrier_init(&context.phase, NULL, workers);
	if (err) {
		err = -err;
		goto out;
	}
	barrier_initialized = true;

	for (i = 0; i < workers; i++) {
		worker_args[i].context = &context;
		worker_args[i].worker = i;
		err = pthread_create(&threads[i], NULL, stress_thread_worker,
				     &worker_args[i]);
		if (err) {
			err = -err;
			break;
		}
		created++;
	}
	pthread_mutex_lock(&context.gate.lock);
	if (created == workers)
		context.gate.go = true;
	else
		context.gate.abort = true;
	pthread_cond_broadcast(&context.gate.cond);
	pthread_mutex_unlock(&context.gate.lock);
	for (i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != workers)
		goto out;

	pthread_mutex_lock(&context.error_lock);
	err = context.error;
	pthread_mutex_unlock(&context.error_lock);
	if (!err)
		err = write_stress_results(result_manifest, context.matrix,
					   workers, iterations);

out:
	if (barrier_initialized)
		pthread_barrier_destroy(&context.phase);
	free(threads);
	free(worker_args);
	free(context.fds);
	va_allocator_destroy(&context.allocator);
	free(context.matrix);
	pthread_cond_destroy(&context.gate.cond);
	pthread_mutex_destroy(&context.gate.lock);
	pthread_mutex_destroy(&context.error_lock);
	return err;
}

int main(int argc, char **argv)
{
	enum {
		OPT_WORKERS = 1000,
		OPT_ITERATIONS,
		OPT_CMB_SIZE,
		OPT_VA_GRANULARITY,
		OPT_BACKING_PAGE_SIZE,
		OPT_RESULT_MANIFEST,
	};
	const char *topology = NULL;
	const char *manifest = NULL;
	const char *result_manifest = NULL;
	enum run_mode mode = MODE_SINGLE;
	bool mode_set = false;
	struct test_case *tests = NULL;
	struct stress_case *stress_tests = NULL;
	size_t count = 0;
	unsigned int workers = 0;
	unsigned int iterations = 0;
	unsigned long cmb_size = 0;
	unsigned long va_granularity = 0;
	unsigned long backing_page_size = 0;
	int dev_fd = -1;
	int option;
	int err;
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "manifest", required_argument, NULL, 'm' },
		{ "mode", required_argument, NULL, 'M' },
		{ "workers", required_argument, NULL, OPT_WORKERS },
		{ "iterations", required_argument, NULL, OPT_ITERATIONS },
		{ "cmb-size", required_argument, NULL, OPT_CMB_SIZE },
		{ "va-granularity", required_argument, NULL, OPT_VA_GRANULARITY },
		{ "backing-page-size", required_argument, NULL,
		  OPT_BACKING_PAGE_SIZE },
		{ "result-manifest", required_argument, NULL,
		  OPT_RESULT_MANIFEST },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	while ((option = getopt_long(argc, argv, "t:m:M:h", options, NULL)) !=
	       -1) {
		switch (option) {
		case 't':
			topology = optarg;
			break;
		case 'm':
			manifest = optarg;
			break;
		case 'M':
			mode_set = true;
			if (!strcmp(optarg, "single"))
				mode = MODE_SINGLE;
			else if (!strcmp(optarg, "queued"))
				mode = MODE_QUEUED;
			else if (!strcmp(optarg, "threaded"))
				mode = MODE_THREADED;
			else if (!strcmp(optarg, "stress"))
				mode = MODE_STRESS;
			else {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			break;
		case OPT_WORKERS:
			err = parse_uint(optarg, &workers);
			if (err)
				goto invalid_option;
			break;
		case OPT_ITERATIONS:
			err = parse_uint(optarg, &iterations);
			if (err)
				goto invalid_option;
			break;
		case OPT_CMB_SIZE:
			err = parse_ulong(optarg, &cmb_size);
			if (err)
				goto invalid_option;
			break;
		case OPT_VA_GRANULARITY:
			err = parse_ulong(optarg, &va_granularity);
			if (err)
				goto invalid_option;
			break;
		case OPT_BACKING_PAGE_SIZE:
			err = parse_ulong(optarg, &backing_page_size);
			if (err)
				goto invalid_option;
			break;
		case OPT_RESULT_MANIFEST:
			result_manifest = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (!topology || !manifest || !mode_set || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (mode == MODE_STRESS) {
		if (!workers || !iterations || !cmb_size || !va_granularity ||
		    !backing_page_size || !result_manifest) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		err = load_stress_manifest(manifest, &stress_tests, &count);
		if (err) {
			fprintf(stderr, "load stress manifest failed: %s\n",
				strerror(-err));
			return EXIT_FAILURE;
		}
		err = run_stress(topology, stress_tests, count, workers,
				 iterations, cmb_size, va_granularity,
				 backing_page_size, result_manifest);
		if (err)
			fprintf(stderr, "stress run failed: %s (%d)\n",
				strerror(-err), err);
		free_stress_cases(stress_tests, count);
		return err ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (workers || iterations || cmb_size || va_granularity ||
	    backing_page_size || result_manifest) {
		fprintf(stderr, "stress-specific options require --mode stress\n");
		return EXIT_FAILURE;
	}

	err = load_manifest(manifest, &tests, &count);
	if (err) {
		fprintf(stderr, "load manifest failed: %s\n", strerror(-err));
		return EXIT_FAILURE;
	}
	dev_fd = new_p2p_fd();
	if (dev_fd < 0) {
		err = dev_fd;
		fprintf(stderr, "new_p2p_fd failed: %s\n", strerror(-err));
		goto out;
	}
	err = add_topo(dev_fd, topology);
	if (err) {
		fprintf(stderr, "add_topo %s failed: %s\n", topology,
			strerror(-err));
		goto out;
	}

	switch (mode) {
	case MODE_SINGLE:
		err = run_single(dev_fd, tests, count);
		break;
	case MODE_QUEUED:
		err = run_queued(dev_fd, tests, count);
		break;
	case MODE_THREADED:
		err = run_threaded(dev_fd, tests, count);
		break;
	case MODE_STRESS:
		err = -EINVAL;
		break;
	}
	if (err)
		fprintf(stderr, "test run failed: %s (%d)\n", strerror(-err),
			err);

out:
	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	free_cases(tests, count);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;

invalid_option:
	usage(argv[0]);
	return EXIT_FAILURE;
}
