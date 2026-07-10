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
#include <stdint.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#include "p2p_dev_uapi.h"
#include "file_p2p_api.h"

#define FILE_P2P_SECTOR_SIZE 512U
#define FILE_P2P_DEFAULT_BLOCK_SIZE 4096U
#define FILE_P2P_UNSUPPORTED_FIEMAP_FLAGS \
	(FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | \
	 FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED | \
	 FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE | \
	 FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN)

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
		return err;
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

static int is_sector_aligned(unsigned long long value)
{
	return !(value & (FILE_P2P_SECTOR_SIZE - 1));
}

static int trim_fiemap_extents(struct fiemap_extent *extents,
			       unsigned int ext_num, unsigned long offset,
			       unsigned long size, unsigned int *trimmed_ext_num,
			       unsigned long long *total_size)
{
	unsigned long long request_start = offset;
	unsigned long long request_end = request_start + size;
	unsigned long long total = 0;
	unsigned int out = 0;
	unsigned int i;

	if (request_end < request_start)
		return -EOVERFLOW;

	for (i = 0; i < ext_num; i++) {
		struct fiemap_extent extent = extents[i];
		unsigned long long extent_start = extent.fe_logical;
		unsigned long long extent_end = extent_start + extent.fe_length;
		unsigned long long trim_start;
		unsigned long long trim_end;

		if (extent_end < extent_start)
			return -EOVERFLOW;
		if (extent_end <= request_start || extent_start >= request_end)
			continue;
		if (extent.fe_flags & FILE_P2P_UNSUPPORTED_FIEMAP_FLAGS) {
			fprintf(stderr,
				"unsupported FIEMAP extent flags 0x%x at extent %u\n",
				extent.fe_flags, i);
			return -EOPNOTSUPP;
		}

		trim_start = extent_start > request_start ?
			     extent_start : request_start;
		trim_end = extent_end < request_end ? extent_end : request_end;
		if (trim_end <= trim_start)
			continue;

		extent.fe_physical += trim_start - extent_start;
		extent.fe_logical = trim_start;
		extent.fe_length = trim_end - trim_start;
		if (!is_sector_aligned(extent.fe_logical) ||
		    !is_sector_aligned(extent.fe_physical) ||
		    !is_sector_aligned(extent.fe_length)) {
			fprintf(stderr,
				"unaligned FIEMAP extent logical 0x%llx physical 0x%llx length 0x%llx\n",
				extent.fe_logical, extent.fe_physical,
				extent.fe_length);
			return -EINVAL;
		}
		extents[out++] = extent;
		total += extent.fe_length;
	}

	*trimmed_ext_num = out;
	*total_size = total;
	return 0;
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
		if (!is_sector_aligned(offset) || !is_sector_aligned(size))
			return -EINVAL;

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

	err = trim_fiemap_extents(exts->fm_extents, exts->fm_mapped_extents,
				  offset, size, ext_num_out, total_size_out);
	if (err) {
		free(exts);
		return err;
	}
	*exts_out = exts;
	return 0;
}

static int get_iov_size(const struct p2p_iov *iov, unsigned int iov_nr,
			unsigned long *size_out)
{
	unsigned long size = 0;
	unsigned int i;

	if (!iov || !iov_nr)
		return -EINVAL;

	for (i = 0; i < iov_nr; i++) {
		if (!iov[i].size ||
		    ((iov[i].addr | iov[i].size) &
		     (FILE_P2P_SECTOR_SIZE - 1)) ||
		    ULONG_MAX - iov[i].addr < iov[i].size ||
		    ULONG_MAX - size < iov[i].size)
			return -EINVAL;
		size += iov[i].size;
	}

	*size_out = size;
	return 0;
}

static int submit_read(int dev_fd, const struct read_parameter *param,
		       unsigned long ioctl_cmd)
{
	struct p2p_read_param *read;
	struct fiemap *exts = NULL;
	struct stat file_stat;
	unsigned long read_size;
	unsigned long long total_size = 0;
	size_t request_size;
	unsigned int ext_num = 0;
	const char *name;
	int file_fd = -1;
	int err = 0;

	if (!param || !param->file_name)
		return -EINVAL;
	err = get_iov_size(param->iov, param->iov_nr, &read_size);
	if (err)
		return err;
	if (ULONG_MAX - param->file_offset < read_size)
		return -EOVERFLOW;

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

	err = prepare_read_extents(file_fd, &file_stat, param->file_offset,
				   read_size, &exts, &ext_num, &total_size);
	if (err) {
		fprintf(stderr, "prepare extents failed, errno: %d\n", err);
		goto close_fds_out;
	}

	if (!ext_num || total_size < read_size) {
		err = -ENODATA;
		fprintf(stderr, "extent size %llu < IOV size %lu\n", total_size,
			read_size);
		goto free_ext_out;
	}

	if (__builtin_mul_overflow((size_t)ext_num,
				   sizeof(read->extents[0]), &request_size) ||
	    __builtin_add_overflow(request_size, sizeof(*read),
				   &request_size)) {
		err = -E2BIG;
		goto free_ext_out;
	}
	read = malloc(request_size);
	if (read == NULL) {
		err = -ENOMEM;
		fprintf(stderr, "malloc p2p_read_param failed, errno: %d\n", err);
		goto free_ext_out;
	}

	read->file_fd = file_fd;
	read->nsid = 1;
	read->host_pid = getpid();
	read->iov = param->iov;
	read->iov_nr = param->iov_nr;
	read->ext_nr = ext_num;
	memcpy(read->extents, exts->fm_extents,
	       ext_num * sizeof(read->extents[0]));

	err = ioctl(dev_fd, ioctl_cmd, read);
	if (err < 0) {
		err = -errno;
		fprintf(stderr, "read ioctl 0x%lx failed, errno: %d\n",
			ioctl_cmd, -err);
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

int read_file(int dev_fd, struct read_parameter *param)
{
	return submit_read(dev_fd, param, IOCTL_READ_FILE);
}

int read_file_batch(int dev_fd, struct read_parameter *params, int param_num)
{
	int i;
	int err;

	if (!params || param_num <= 0)
		return -EINVAL;
	for (i = 0; i < param_num; i++) {
		err = submit_read(dev_fd, &params[i], IOCTL_READ_FILE_BATCH);
		if (err)
			return err;
	}
	return 0;
}

int drain_read(int dev_fd)
{
	if (ioctl(dev_fd, IOCTL_DRAIN_READ) < 0)
		return -errno;
	return 0;
}
