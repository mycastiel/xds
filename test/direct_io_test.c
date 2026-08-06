#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT 4096UL
#define GUARD_SIZE ALIGNMENT
#define CRC32_POLY_LE 0xedb88320U

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --mode <prepare|verify> --device <bdev> "
		"--source-offset <bytes> --target-offset <bytes> "
		"--length <bytes> --seed <value>\n",
		program);
}

static int parse_u64(const char *text, uint64_t *value)
{
	unsigned long long parsed;
	char *end;

	if (!text || !*text || *text == '-')
		return -EINVAL;
	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno == ERANGE)
		return -ERANGE;
	if (end == text || *end)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static void fill_pattern(unsigned char *buffer, size_t length, uint64_t seed)
{
	size_t i;

	for (i = 0; i < length; i++)
		buffer[i] = (i * 131 + (i >> 8) * 17 + seed * 29) & 0xff;
}

static uint32_t crc32(const unsigned char *data, size_t length)
{
	uint32_t crc = UINT32_MAX;

	while (length--) {
		unsigned int bit;

		crc ^= *data++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			      ((crc & 1) ? CRC32_POLY_LE : 0);
	}
	return crc ^ UINT32_MAX;
}

static int direct_write(int fd, const void *buffer, size_t length,
			uint64_t offset)
{
	ssize_t bytes;

	bytes = pwrite(fd, buffer, length, (off_t)offset);
	if (bytes < 0)
		return -errno;
	if ((size_t)bytes != length)
		return -EIO;
	return 0;
}

static int direct_read(int fd, void *buffer, size_t length, uint64_t offset)
{
	ssize_t bytes;

	bytes = pread(fd, buffer, length, (off_t)offset);
	if (bytes < 0)
		return -errno;
	if ((size_t)bytes != length)
		return -EIO;
	return 0;
}

static void build_target_window(unsigned char *window,
				const unsigned char *payload, size_t length,
				uint64_t seed, int expected)
{
	fill_pattern(window, GUARD_SIZE, seed + 1);
	if (expected)
		memcpy(window + GUARD_SIZE, payload, length);
	else
		fill_pattern(window + GUARD_SIZE, length, seed + 2);
	fill_pattern(window + GUARD_SIZE + length, GUARD_SIZE, seed + 3);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "mode", required_argument, NULL, 'm' },
		{ "device", required_argument, NULL, 'd' },
		{ "source-offset", required_argument, NULL, 's' },
		{ "target-offset", required_argument, NULL, 't' },
		{ "length", required_argument, NULL, 'l' },
		{ "seed", required_argument, NULL, 'e' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *device = NULL;
	const char *mode = NULL;
	unsigned char *actual = NULL;
	unsigned char *expected = NULL;
	unsigned char *payload = NULL;
	uint64_t source_offset = UINT64_MAX;
	uint64_t target_offset = UINT64_MAX;
	uint64_t length_u64 = 0;
	uint64_t seed = 0;
	size_t window_size;
	size_t length;
	int fd = -1;
	int option;
	int err;

	while ((option = getopt_long(argc, argv, "m:d:s:t:l:e:h", options,
				     NULL)) != -1) {
		switch (option) {
		case 'm':
			mode = optarg;
			break;
		case 'd':
			device = optarg;
			break;
		case 's':
			if (parse_u64(optarg, &source_offset))
				goto invalid;
			break;
		case 't':
			if (parse_u64(optarg, &target_offset))
				goto invalid;
			break;
		case 'l':
			if (parse_u64(optarg, &length_u64))
				goto invalid;
			break;
		case 'e':
			if (parse_u64(optarg, &seed))
				goto invalid;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			goto invalid;
		}
	}

	if (!mode || !device || optind != argc || !length_u64 ||
	    length_u64 > SIZE_MAX || source_offset == UINT64_MAX ||
	    target_offset == UINT64_MAX ||
	    (source_offset | target_offset | length_u64) & (ALIGNMENT - 1) ||
	    target_offset < GUARD_SIZE)
		goto invalid;
	if (strcmp(mode, "prepare") && strcmp(mode, "verify"))
		goto invalid;

	length = (size_t)length_u64;
	if (__builtin_add_overflow(length, 2 * GUARD_SIZE, &window_size))
		goto invalid;
	if (posix_memalign((void **)&payload, ALIGNMENT, length) ||
	    posix_memalign((void **)&expected, ALIGNMENT, window_size) ||
	    posix_memalign((void **)&actual, ALIGNMENT, window_size)) {
		err = -ENOMEM;
		goto out;
	}

	fill_pattern(payload, length, seed);
	build_target_window(expected, payload, length, seed,
			    !strcmp(mode, "verify"));

	fd = open(device, (!strcmp(mode, "prepare") ? O_RDWR : O_RDONLY) |
		  O_DIRECT | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		goto out;
	}

	if (!strcmp(mode, "prepare")) {
		err = direct_write(fd, payload, length, source_offset);
		if (!err)
			err = direct_write(fd, expected, window_size,
					   target_offset - GUARD_SIZE);
		if (!err && fsync(fd))
			err = -errno;
		if (!err)
			printf("expected_crc32=0x%08" PRIx32 "\n",
			       crc32(payload, length));
	} else {
		err = direct_read(fd, actual, window_size,
				  target_offset - GUARD_SIZE);
		if (!err && memcmp(actual, expected, window_size))
			err = -EILSEQ;
		if (!err)
			printf("direct-readback-pass length=0x%zx\n", length);
	}

out:
	if (err)
		fprintf(stderr, "%s failed: %s\n", mode ? mode : "operation",
			strerror(-err));
	if (fd >= 0)
		close(fd);
	free(actual);
	free(expected);
	free(payload);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;

invalid:
	usage(argv[0]);
	return EXIT_FAILURE;
}
