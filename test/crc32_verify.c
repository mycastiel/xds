#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CRC32_POLY_LE 0xedb88320U
#define CRC32_BUFFER_SIZE (1U << 20)

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s -f <file-or-block-device> [-o <offset>] "
		"[-l <length>]\n",
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

static int get_object_size(int fd, uint64_t *size)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -errno;

	if (S_ISREG(st.st_mode)) {
		if (st.st_size < 0)
			return -EOVERFLOW;
		*size = (uint64_t)st.st_size;
		return 0;
	}

	if (S_ISBLK(st.st_mode)) {
		uint64_t bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) < 0)
			return -errno;
		*size = bytes;
		return 0;
	}

	return -EINVAL;
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

static uint32_t update_crc32(uint32_t crc, const unsigned char *data,
			     size_t length, const uint32_t table[256])
{
	while (length--) {
		crc = (crc >> 8) ^ table[(crc ^ *data) & 0xff];
		data++;
	}

	return crc;
}

static int calculate_crc32(int fd, uint64_t offset, uint64_t length,
			   uint32_t *result)
{
	uint32_t table[256];
	unsigned char *buffer;
	uint64_t done = 0;
	uint32_t crc = UINT32_MAX;
	int err = 0;

	if (!length) {
		*result = crc ^ UINT32_MAX;
		return 0;
	}

	buffer = malloc(CRC32_BUFFER_SIZE);
	if (!buffer)
		return -ENOMEM;

	init_crc32_table(table);
	while (done < length) {
		uint64_t left = length - done;
		size_t to_read = left < CRC32_BUFFER_SIZE ?
				 left : CRC32_BUFFER_SIZE;
		uint64_t position = offset + done;
		ssize_t bytes;

		if (position > INT64_MAX) {
			err = -EOVERFLOW;
			break;
		}

		bytes = pread(fd, buffer, to_read, (off_t)position);
		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			break;
		}
		if (!bytes) {
			err = -ENODATA;
			break;
		}

		crc = update_crc32(crc, buffer, (size_t)bytes, table);
		done += (size_t)bytes;
	}

	free(buffer);
	if (err)
		return err;

	*result = crc ^ UINT32_MAX;
	return 0;
}

int main(int argc, char **argv)
{
	const char *file_name = NULL;
	uint64_t object_size = 0;
	uint64_t offset = 0;
	uint64_t length = 0;
	uint32_t crc;
	bool length_set = false;
	int fd = -1;
	int option;
	int err;

	while ((option = getopt(argc, argv, "f:o:l:h")) != -1) {
		switch (option) {
		case 'f':
			file_name = optarg;
			break;
		case 'o':
			err = parse_u64(optarg, &offset);
			if (err) {
				fprintf(stderr, "invalid offset: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'l':
			err = parse_u64(optarg, &length);
			if (err) {
				fprintf(stderr, "invalid length: %s\n", optarg);
				return EXIT_FAILURE;
			}
			length_set = true;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!file_name || optind != argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(file_name, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		fprintf(stderr, "open %s failed: %s\n", file_name,
			strerror(-err));
		return EXIT_FAILURE;
	}

	err = get_object_size(fd, &object_size);
	if (err) {
		fprintf(stderr, "get size of %s failed: %s\n", file_name,
			strerror(-err));
		goto out;
	}
	if (offset > object_size) {
		err = -ERANGE;
		fprintf(stderr, "offset 0x%" PRIx64
			" exceeds object size 0x%" PRIx64 "\n",
			offset, object_size);
		goto out;
	}
	if (!length_set) {
		length = object_size - offset;
	} else if (length > object_size - offset) {
		err = -ERANGE;
		fprintf(stderr, "range offset 0x%" PRIx64
			" length 0x%" PRIx64
			" exceeds object size 0x%" PRIx64 "\n",
			offset, length, object_size);
		goto out;
	}
	if (offset > INT64_MAX ||
	    (length && length - 1 > (uint64_t)INT64_MAX - offset)) {
		err = -EOVERFLOW;
		fprintf(stderr, "range does not fit off_t\n");
		goto out;
	}

	err = calculate_crc32(fd, offset, length, &crc);
	if (err) {
		fprintf(stderr, "calculate CRC32 for %s failed: %s\n",
			file_name, strerror(-err));
		goto out;
	}

	printf("crc32 file %s offset 0x%" PRIx64 " length 0x%" PRIx64
	       " crc32 0x%08x\n",
	       file_name, offset, length, crc);

out:
	close(fd);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
