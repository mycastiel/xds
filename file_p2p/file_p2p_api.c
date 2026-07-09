#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#include "p2p_dev_uapi.h"
#include "file_p2p_api.h"

#define FILE_P2P_SECTOR_SIZE 512U
#define FILE_P2P_DEFAULT_BLOCK_SIZE 4096U

long long timespec_diff_ms(const struct timespec *start,
			   const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000LL +
	       (end->tv_nsec - start->tv_nsec) / 1000000LL;
}

int close_p2p_fd(int dev_fd)
{
	return close(dev_fd);
}

int new_p2p_fd(void)
{
	int dev_fd;
	int err;

	dev_fd = open("/dev/p2p_device", O_RDWR);
	if (dev_fd < 0) {
		err = -errno;
		fprintf(stderr, "open /dev/p2p_device failed, errno: %d\n",
			err);
		return -err;
	}

	return dev_fd;
}

static int is_power_of_two(unsigned int value)
{
	return value && !(value & (value - 1));
}

static unsigned int get_fiemap_block_size(int file_fd)
{
	int block_size = 0;

	if (!ioctl(file_fd, FIGETBSZ, &block_size) &&
	    block_size >= (int)FILE_P2P_SECTOR_SIZE &&
	    is_power_of_two((unsigned int)block_size))
		return block_size;

	return FILE_P2P_DEFAULT_BLOCK_SIZE;
}

static unsigned long long align_down_u64(unsigned long long value,
					 unsigned int alignment)
{
	return value & ~((unsigned long long)alignment - 1);
}

static unsigned long long align_up_u64(unsigned long long value,
				       unsigned int alignment)
{
	return (value + alignment - 1) & ~((unsigned long long)alignment - 1);
}

static int calc_fiemap_extent_count(unsigned long offset, unsigned long size,
				    unsigned int block_size,
				    unsigned int *count)
{
	unsigned long long block_offset = offset & (block_size - 1);
	unsigned long long blocks;

	blocks = align_up_u64(block_offset + size, block_size) / block_size;
	if (!blocks)
		blocks = 1;
	if (blocks > UINT_MAX)
		return -E2BIG;

	*count = blocks;
	return 0;
}

static struct fiemap *alloc_fiemap(unsigned int extent_count)
{
	return calloc(1, sizeof(struct fiemap) +
		      extent_count * sizeof(struct fiemap_extent));
}

static unsigned int trim_fiemap_extents(struct fiemap_extent *extents,
					unsigned int ext_num,
					unsigned long offset,
					unsigned long size,
					unsigned long long *total_size)
{
	unsigned long long request_start = offset;
	unsigned long long request_end = request_start + size;
	unsigned long long total = 0;
	unsigned int out = 0;
	unsigned int i;

	for (i = 0; i < ext_num; i++) {
		struct fiemap_extent extent = extents[i];
		unsigned long long extent_start = extent.fe_logical;
		unsigned long long extent_end = extent_start + extent.fe_length;
		unsigned long long trim_start;
		unsigned long long trim_end;

		if (extent_end <= request_start || extent_start >= request_end)
			continue;

		trim_start = extent_start > request_start ?
			     extent_start : request_start;
		trim_end = extent_end < request_end ? extent_end : request_end;
		if (trim_end <= trim_start)
			continue;

		extent.fe_physical += trim_start - extent_start;
		extent.fe_logical = trim_start;
		extent.fe_length = trim_end - trim_start;
		extents[out++] = extent;
		total += extent.fe_length;
	}

	*total_size = total;
	return out;
}

static int prepare_read_extents(int file_fd, const struct stat *file_stat,
				unsigned long offset, unsigned long size,
				struct fiemap **exts_out,
				unsigned int *ext_num_out,
				unsigned long long *total_size_out)
{
	struct fiemap *exts;
	unsigned int block_size;
	unsigned int max_num;
	unsigned long long query_start;
	unsigned long long query_offset;
	unsigned long long query_length;
	int err;

	if (S_ISBLK(file_stat->st_mode)) {
		exts = alloc_fiemap(1);
		if (!exts)
			return -ENOMEM;

		exts->fm_extents[0].fe_logical = offset;
		exts->fm_extents[0].fe_physical = offset;
		exts->fm_extents[0].fe_length = size;
		*exts_out = exts;
		*ext_num_out = 1;
		*total_size_out = size;
		return 0;
	}

	if (!S_ISREG(file_stat->st_mode))
		return -EINVAL;

	block_size = get_fiemap_block_size(file_fd);
	err = calc_fiemap_extent_count(offset, size, block_size, &max_num);
	if (err)
		return err;

	exts = alloc_fiemap(max_num);
	if (!exts)
		return -ENOMEM;

	query_start = align_down_u64(offset, block_size);
	query_offset = offset - query_start;
	query_length = align_up_u64(query_offset + size, block_size);

	exts->fm_start = query_start;
	exts->fm_length = query_length;
	exts->fm_flags = 0;
	exts->fm_extent_count = max_num;

	err = ioctl(file_fd, FS_IOC_FIEMAP, exts);
	if (err) {
		err = -errno;
		fprintf(stderr, "ioctl FS_IOC_FIEMAP failed, errno: %d\n", err);
		free(exts);
		return err;
	}

	*ext_num_out = trim_fiemap_extents(exts->fm_extents,
					   exts->fm_mapped_extents,
					   offset, size, total_size_out);
	*exts_out = exts;
	return 0;
}

int read_file(int dev_fd, struct read_parameter *param)
{
	struct read_desc *read;
	struct fiemap *exts;
	struct stat file_stat;
	unsigned long max_size;
	unsigned long long total_size;
	unsigned int ext_num;
	const char *name;
	int file_fd = -1;
	int err = 0;

	name = param->file_name;
	file_fd = open(name, O_RDONLY);
	if (file_fd < 0) {
		err = -errno;
		fprintf(stderr, "open %s failed, errno: %d\n", name, err);
		goto close_fds_out;
	}

	err = fstat(file_fd, &file_stat);
	if (err < 0) {
		err = -errno;
		fprintf(stderr, "fstat %s failed, errno: %d\n", name, err);
		goto close_fds_out;
	}

	max_size = param->size;
	err = prepare_read_extents(file_fd, &file_stat, param->bdev_offset,
				   max_size, &exts, &ext_num, &total_size);
	if (err) {
		fprintf(stderr, "prepare extents failed, errno: %d\n", err);
		goto close_fds_out;
	}

	if (total_size > param->size) {
		err = -EINVAL;
		fprintf(stderr, "total_size %llu > param->size %u\n",
			total_size, param->size);
		goto free_ext_out;
	}

	read = malloc(sizeof(*read) + ext_num * sizeof(read->extents[0]));
	if (read == NULL) {
		err = -ENOMEM;
		fprintf(stderr, "malloc read_desc failed, errno: %d\n", err);
		goto free_ext_out;
	}

	read->desc.hostpid = getpid();
	read->desc.devid = param->devid;
	read->desc.vfid = param->vfid;
	read->desc.addr = param->addr;
	read->desc.size = param->size;
	read->nsid = 1;
	read->file_fd = file_fd;
	read->ext_num = ext_num;
	memcpy(read->extents, exts->fm_extents,
	       ext_num * sizeof(read->extents[0]));

	err = ioctl(dev_fd, IOCTL_READ_FILE, read);
	if (err < 0) {
		err = -errno;
		fprintf(stderr, "ioctl IOCTL_READ_FILE failed, errno: %d\n",
			-err);
		goto free_read_out;
	}

free_read_out:
	free(read);
free_ext_out:
	free(exts);
close_fds_out:
	if (file_fd >= 0)
		close(file_fd);

	return err;
}

int read_file_batch(int dev_fd, struct read_parameter *param, int param_num)
{
	struct read_desc_ba *read;
	struct fiemap *exts;
	struct stat file_stat;
	unsigned long max_size;
	unsigned long long total_size;
	unsigned int ext_num;
	const char *name;
	int j;
	int file_fd = -1;
	int err = 0;

	name = param[0].file_name;
	file_fd = open(name, O_RDONLY);
	if (file_fd < 0) {
		err = -errno;
		fprintf(stderr, "open %s failed, errno: %d\n", name, err);
		goto close_fds_out;
	}

	err = fstat(file_fd, &file_stat);
	if (err < 0) {
		err = -errno;
		fprintf(stderr, "fstat %s failed, errno: %d\n", name, err);
		goto close_fds_out;
	}

	max_size = (unsigned long)param[0].size * param_num;
	err = prepare_read_extents(file_fd, &file_stat, param[0].bdev_offset,
				   max_size, &exts, &ext_num, &total_size);
	if (err) {
		fprintf(stderr, "prepare extents failed, errno: %d\n", err);
		goto close_fds_out;
	}

	if (total_size > max_size) {
		err = -EINVAL;
		fprintf(stderr, "total_size %llu > max_size %lu\n", total_size,
			max_size);
		goto free_ext_out;
	}

	read = calloc(1, sizeof(*read) + ext_num * sizeof(read->extents[0]));
	if (read == NULL) {
		err = -ENOMEM;
		fprintf(stderr, "malloc read_desc failed, errno: %d\n", err);
		goto free_ext_out;
	}

	read->desc.hostpid = getpid();
	read->desc.devid = param->devid;
	read->desc.vfid = param->vfid;
	read->desc.count = param_num;
	read->nsid = 1;
	read->file_fd = file_fd;
	read->ext_num = ext_num;
	memcpy(read->extents, exts->fm_extents,
	       ext_num * sizeof(read->extents[0]));

	read->desc.addr = malloc(param_num * sizeof(unsigned long));
	read->desc.size = malloc(param_num * sizeof(unsigned long));
	if (read->desc.addr == NULL || read->desc.size == NULL) {
		err = -ENOMEM;
		fprintf(stderr, "malloc read_desc desc failed, errno: %d\n",
			err);
		goto free_read_out;
	}
	for (j = 0; j < param_num; j++) {
		read->desc.addr[j] = param[j].addr;
		read->desc.size[j] = param[j].size;
	}

	err = ioctl(dev_fd, IOCTL_READ_FILE_BATCH, read);
	if (err < 0) {
		err = -errno;
		fprintf(stderr,
			"ioctl IOCTL_READ_FILE_BATCH failed, errno: %d\n",
			-err);
		goto free_read_out;
	}

free_read_out:
	if (read->desc.addr != NULL)
		free(read->desc.addr);
	if (read->desc.size != NULL)
		free(read->desc.size);
	free(read);
free_ext_out:
	free(exts);
close_fds_out:
	if (file_fd >= 0)
		close(file_fd);

	return err;
}
