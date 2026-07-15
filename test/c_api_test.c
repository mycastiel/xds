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

enum run_mode {
	MODE_SINGLE,
	MODE_QUEUED,
	MODE_THREADED,
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
};

struct worker {
	struct start_gate *gate;
	struct test_case *test;
	int dev_fd;
};

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --topology <block-device> --manifest <tsv> "
		"--mode <single|queued|threaded>\n",
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
			err = drain_read(dev_fd);
			if (err) {
				fprintf(stderr, "drain after %s failed: %d\n",
					tests[i].id, err);
				return err;
			}
		}
	}
	return drain_read(dev_fd);
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
	return drain_read(dev_fd);
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
	err = drain_read(dev_fd);

out:
	free(threads);
	free(workers);
	pthread_cond_destroy(&gate.cond);
	pthread_mutex_destroy(&gate.lock);
	return err;
}

int main(int argc, char **argv)
{
	const char *topology = NULL;
	const char *manifest = NULL;
	enum run_mode mode = MODE_SINGLE;
	bool mode_set = false;
	struct test_case *tests = NULL;
	size_t count = 0;
	int dev_fd = -1;
	int option;
	int err;
	static const struct option options[] = {
		{ "topology", required_argument, NULL, 't' },
		{ "manifest", required_argument, NULL, 'm' },
		{ "mode", required_argument, NULL, 'M' },
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
			else {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
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
	}
	if (err)
		fprintf(stderr, "test run failed: %s (%d)\n", strerror(-err),
			err);

out:
	if (dev_fd >= 0)
		close_p2p_fd(dev_fd);
	free_cases(tests, count);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
