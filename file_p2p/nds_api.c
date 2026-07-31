/*
 * NDS — a GDS-aligned userspace API for NPU HBM ↔ NVMe P2P I/O.
 *
 * NDS is intentionally self-contained: it talks to the kernel driver
 * (/dev/p2p_device) through p2p_dev_uapi.h ioctls directly and does NOT link
 * against file_p2p_api. The two userspace APIs evolve independently on top of
 * the same kernel UAPI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>

#include <linux/dm-ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#include "nds_api.h"
#include "p2p_dev_uapi.h"

#define NDS_P2P_DEVICE "/dev/p2p_device"
#define NDS_SECTOR_SIZE 512U
#define NDS_SECTORS_PER_KB (1024U / NDS_SECTOR_SIZE)
#define NDS_DEFAULT_BLOCK_SIZE 4096U
#define NDS_DM_CONTROL "/dev/mapper/control"
#define NDS_DM_BUFFER_SIZE (16U << 10)
#define NDS_UNSUPPORTED_FIEMAP_FLAGS \
	(FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | \
	 FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED | \
	 FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE | \
	 FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN)

struct nds_reg_entry {
	struct nds_reg_entry *next;
	void *addr;
	uint64_t size;
	uint64_t handle;
};

struct nds_state {
	int book_fd;
	struct nds_reg_entry *regs;
};

struct nds_io_ctx {
	int p2p_fd;
	unsigned int max_io_nr;
	unsigned int inflight;
};

static struct nds_state g_state = { .book_fd = -1 };

static int is_init(void)
{
	return g_state.book_fd >= 0;
}

/* ------------------------------------------------------------------ */
/* p2p_device fd lifecycle                                            */
/* ------------------------------------------------------------------ */

static int nds_open_dev(void)
{
	int fd = open(NDS_P2P_DEVICE, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		int err = -errno;
		fprintf(stderr, "nds: open %s failed, errno: %d\n",
			NDS_P2P_DEVICE, err);
		return err;
	}
	return fd;
}

/* ------------------------------------------------------------------ */
/* sysfs / block-device helpers                                       */
/* ------------------------------------------------------------------ */

static int parse_devt(const char *text, dev_t *devt)
{
	unsigned int dev_major;
	unsigned int dev_minor;
	char trailing;

	if (sscanf(text, "%u:%u %c", &dev_major, &dev_minor, &trailing) != 2)
		return -EINVAL;
	*devt = makedev(dev_major, dev_minor);
	return 0;
}

static int sysfs_path(char *path, size_t size, dev_t devt,
		      const char *suffix)
{
	int len;

	len = snprintf(path, size, "/sys/dev/block/%u:%u/%s",
		       major(devt), minor(devt), suffix);
	if (len < 0)
		return -errno;
	if ((size_t)len >= size)
		return -ENAMETOOLONG;
	return 0;
}

static int path_exists(dev_t devt, const char *suffix)
{
	char path[PATH_MAX];
	struct stat st;
	int err;

	err = sysfs_path(path, sizeof(path), devt, suffix);
	if (err)
		return 0;
	return !stat(path, &st);
}

static int validate_nvme_component(dev_t devt)
{
	if (path_exists(devt, "partition") ||
	    !path_exists(devt, "device/subsysnqn") ||
	    path_exists(devt, "device/iopolicy"))
		return -EOPNOTSUPP;
	return 0;
}

static int read_text_at(int dir_fd, const char *path, char *buf, size_t size)
{
	ssize_t got;
	int fd;
	int err;

	fd = openat(dir_fd, path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	got = read(fd, buf, size - 1);
	if (got < 0) {
		err = -errno;
		close(fd);
		return err;
	}
	if ((size_t)got == size - 1) {
		close(fd);
		return -E2BIG;
	}
	close(fd);
	buf[got] = '\0';
	return 0;
}

static int read_text(const char *path, char *buf, size_t size)
{
	return read_text_at(AT_FDCWD, path, buf, size);
}

static int read_sysfs_text(dev_t devt, const char *suffix, char *buf,
			   size_t size)
{
	char path[PATH_MAX];
	int err;

	err = sysfs_path(path, sizeof(path), devt, suffix);
	if (err)
		return err;
	return read_text(path, buf, size);
}

static int read_sysfs_u64(dev_t devt, const char *suffix,
			  unsigned long long *value)
{
	char buf[64];
	int err;

	err = read_sysfs_text(devt, suffix, buf, sizeof(buf));
	if (err)
		return err;
	*value = strtoull(buf, NULL, 10);
	return 0;
}

static int validate_nvme_component_input(dev_t devt)
{
	char parent_devt[64];
	dev_t whole_dev;
	int err;

	if (!path_exists(devt, "partition"))
		return validate_nvme_component(devt);

	err = read_sysfs_text(devt, "../dev", parent_devt,
			      sizeof(parent_devt));
	if (err)
		return err;
	err = parse_devt(parent_devt, &whole_dev);
	if (err)
		return err;
	return validate_nvme_component(whole_dev);
}

static int get_block_size_sectors(const char *dev, uint64_t *size_sectors)
{
	uint64_t size_bytes;
	int fd;
	int err;

	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, BLKGETSIZE64, &size_bytes) < 0)
		err = -errno;
	else
		err = 0;
	close(fd);
	if (err)
		return err;
	if (!size_bytes || size_bytes % NDS_SECTOR_SIZE) {
		fprintf(stderr, "nds: invalid block-device size %llu for %s\n",
			(unsigned long long)size_bytes, dev);
		return -EINVAL;
	}

	*size_sectors = size_bytes / NDS_SECTOR_SIZE;
	return 0;
}

/* ------------------------------------------------------------------ */
/* topology discovery                                                 */
/* ------------------------------------------------------------------ */

static int discover_nvme(const char *dev, dev_t top_dev,
			 struct topo_user_cfg *cfg)
{
	uint64_t size_sectors = 0;
	int err;

	err = validate_nvme_component_input(top_dev);
	if (err)
		return err;
	err = get_block_size_sectors(dev, &size_sectors);
	if (err)
		return err;

	memcpy(cfg->name, "nvme", sizeof("nvme"));
	cfg->nr_devs = 1;
	cfg->top_dev = top_dev;
	cfg->bdevs[0].dev_id = top_dev;
	cfg->bdevs[0].size_sector = size_sectors;
	cfg->bdevs[0].start_sector = 0;
	return 0;
}

static int parse_linear_params(const char *params, dev_t *devt,
			       unsigned long long *start_sector)
{
	unsigned int dev_major;
	unsigned int dev_minor;
	int consumed = 0;

	if (sscanf(params, "%u:%u %llu %n", &dev_major, &dev_minor,
		   start_sector, &consumed) != 3)
		return -EINVAL;
	while (isspace((unsigned char)params[consumed]))
		consumed++;
	if (params[consumed])
		return -EINVAL;

	*devt = makedev(dev_major, dev_minor);
	return validate_nvme_component_input(*devt);
}

static int dm_table_query(dev_t top_dev, void **buffer_out)
{
	struct dm_ioctl *dmi;
	void *buffer;
	int control_fd;
	int err;

	control_fd = open(NDS_DM_CONTROL, O_RDWR | O_CLOEXEC);
	if (control_fd < 0) {
		err = -errno;
		fprintf(stderr, "nds: open %s failed, errno: %d\n",
			NDS_DM_CONTROL, err);
		return err;
	}

	buffer = calloc(1, NDS_DM_BUFFER_SIZE);
	if (!buffer) {
		err = -ENOMEM;
		fprintf(stderr,
			"nds: allocate device-mapper table buffer failed: %d\n",
			err);
		goto out;
	}
	dmi = buffer;
	dmi->version[0] = 4;
	/* DM_TABLE_STATUS has existed since interface version 4.0. */
	dmi->version[1] = 0;
	dmi->version[2] = 0;
	dmi->data_size = NDS_DM_BUFFER_SIZE;
	dmi->data_start = sizeof(*dmi);
	dmi->flags = DM_STATUS_TABLE_FLAG;
	dmi->dev = top_dev;

	if (ioctl(control_fd, DM_TABLE_STATUS, dmi) < 0) {
		err = -errno;
		fprintf(stderr, "nds: DM_TABLE_STATUS failed, errno: %d\n",
			err);
		goto out;
	}
	if (dmi->flags & DM_BUFFER_FULL_FLAG) {
		err = -E2BIG;
		fprintf(stderr, "nds: device-mapper table exceeds %u bytes\n",
			NDS_DM_BUFFER_SIZE);
		goto out;
	}

	*buffer_out = buffer;
	buffer = NULL;
	err = 0;
out:
	free(buffer);
	close(control_fd);
	return err;
}

static int discover_linear(const char *dev, dev_t top_dev,
			   struct topo_user_cfg *cfg)
{
	struct dm_target_spec *first;
	struct dm_target_spec *spec;
	struct dm_ioctl *dmi;
	unsigned long long logical_end = 0;
	char *limit;
	void *buffer = NULL;
	unsigned int i;
	int err;

	err = dm_table_query(top_dev, &buffer);
	if (err) {
		fprintf(stderr,
			"nds: discover linear topology for %s failed: %d\n",
			dev, err);
		return err;
	}
	dmi = buffer;
	if (!(dmi->flags & DM_ACTIVE_PRESENT_FLAG) || !dmi->target_count ||
	    dmi->target_count > P2P_TOPO_MAX_BDEVS ||
	    dmi->data_size < sizeof(*dmi) ||
	    dmi->data_start < sizeof(*dmi) || dmi->data_start >= dmi->data_size) {
		err = dmi->target_count > P2P_TOPO_MAX_BDEVS ? -E2BIG : -EINVAL;
		goto out;
	}

	limit = (char *)buffer + dmi->data_size;
	first = (void *)((char *)buffer + dmi->data_start);
	spec = first;
	strcpy(cfg->name, "linear");
	cfg->nr_devs = dmi->target_count;
	cfg->top_dev = top_dev;

	for (i = 0; i < cfg->nr_devs; i++) {
		struct topo_user_bdev *entry = &cfg->bdevs[i];
		unsigned long long start_sector;
		char *params;
		char *params_end;
		dev_t component;

		if ((char *)(spec + 1) > limit ||
		    !memchr(spec->target_type, '\0', sizeof(spec->target_type)) ||
		    strcmp(spec->target_type, "linear") || !spec->length ||
		    spec->sector_start != logical_end) {
			err = -EINVAL;
			goto out;
		}
		params = (char *)(spec + 1);
		if (i + 1 < cfg->nr_devs) {
			if (!spec->next || (char *)first + spec->next <= params ||
			    (char *)first + spec->next > limit) {
				err = -EINVAL;
				goto out;
			}
			params_end = (char *)first + spec->next;
		} else {
			params_end = limit;
		}
		if (!memchr(params, '\0', params_end - params)) {
			err = -EINVAL;
			goto out;
		}
		err = parse_linear_params(params, &component, &start_sector);
		if (err)
			goto out;
		entry->dev_id = component;
		entry->size_sector = spec->length;
		entry->start_sector = start_sector;
		if (__builtin_add_overflow(logical_end,
					   (unsigned long long)spec->length,
					   &logical_end)) {
			err = -EINVAL;
			goto out;
		}
		if (i + 1 < cfg->nr_devs)
			spec = (void *)((char *)first + spec->next);
	}
	err = 0;
out:
	free(buffer);
	return err;
}

struct raid0_member {
	int slot;
	struct topo_user_bdev bdev;
};

static int compare_raid0_member_slot(const void *left, const void *right)
{
	const struct raid0_member *a = left;
	const struct raid0_member *b = right;

	return a->slot - b->slot;
}

static int read_member_value(int member_fd, const char *name,
			     unsigned long long *value)
{
	char text[64];
	int err;

	err = read_text_at(member_fd, name, text, sizeof(text));
	if (err)
		return err;
	*value = strtoull(text, NULL, 10);
	return 0;
}

static int read_raid0_member(int md_fd, const char *name,
			     unsigned int raid_disks,
			     struct raid0_member *member)
{
	unsigned long long start_sector;
	unsigned long long size_kb;
	char state[128];
	char block_dev[64];
	char slot_text[64];
	dev_t component;
	int slot;
	int member_fd;
	int err;

	member_fd = openat(md_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (member_fd < 0)
		return -errno;
	err = read_text_at(member_fd, "state", state, sizeof(state));
	if (err)
		goto out;
	state[strcspn(state, "\r\n")] = '\0';
	if (strcmp(state, "in_sync")) {
		err = 0;
		goto out;
	}

	err = read_text_at(member_fd, "slot", slot_text, sizeof(slot_text));
	if (err)
		goto out;
	slot_text[strcspn(slot_text, "\r\n")] = '\0';
	if (!strcmp(slot_text, "none")) {
		err = 0;
		goto out;
	}
	slot = atoi(slot_text);
	if (slot < 0 || slot >= (int)raid_disks) {
		err = -EINVAL;
		goto out;
	}
	err = read_member_value(member_fd, "offset", &start_sector);
	if (err)
		goto out;
	err = read_member_value(member_fd, "size", &size_kb);
	if (err)
		goto out;
	if (!size_kb) {
		err = -EINVAL;
		goto out;
	}
	err = read_text_at(member_fd, "block/dev", block_dev,
			   sizeof(block_dev));
	if (err)
		goto out;
	err = parse_devt(block_dev, &component);
	if (err)
		goto out;
	err = validate_nvme_component_input(component);
	if (err)
		goto out;

	member->slot = slot;
	member->bdev.dev_id = component;
	member->bdev.size_sector = size_kb * NDS_SECTORS_PER_KB;
	member->bdev.start_sector = start_sector;
	err = 1;
out:
	close(member_fd);
	return err;
}

static int discover_raid0_members(dev_t top_dev, unsigned int raid_disks,
				  struct topo_user_cfg *cfg)
{
	struct raid0_member members[P2P_TOPO_MAX_BDEVS];
	char md_path[PATH_MAX];
	struct dirent *dirent;
	unsigned int active = 0;
	unsigned int i;
	DIR *dir;
	int dir_fd;
	int err;

	err = sysfs_path(md_path, sizeof(md_path), top_dev, "md");
	if (err)
		return err;
	dir_fd = open(md_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir_fd < 0)
		return -errno;
	dir = fdopendir(dir_fd);
	if (!dir) {
		err = -errno;
		close(dir_fd);
		return err;
	}

	for (;;) {
		struct raid0_member member;

		errno = 0;
		dirent = readdir(dir);
		if (!dirent) {
			if (errno) {
				err = -errno;
				goto out;
			}
			break;
		}
		if (strncmp(dirent->d_name, "dev-", 4))
			continue;
		err = read_raid0_member(dirfd(dir), dirent->d_name, raid_disks,
					&member);
		if (err < 0)
			goto out;
		if (!err)
			continue;
		if (active >= raid_disks) {
			err = -EINVAL;
			goto out;
		}
		members[active++] = member;
	}
	if (active != raid_disks) {
		fprintf(stderr, "nds: RAID0 has %u in-sync members, expected %u\n",
			active, raid_disks);
		err = -EINVAL;
		goto out;
	}

	qsort(members, active, sizeof(members[0]), compare_raid0_member_slot);
	for (i = 0; i < active; i++) {
		if (members[i].slot != (int)i) {
			fprintf(stderr,
				"nds: RAID0 slot %u is missing or duplicated\n", i);
			err = -EINVAL;
			goto out;
		}
		cfg->bdevs[i] = members[i].bdev;
	}
	err = 0;
out:
	closedir(dir);
	return err;
}

static int discover_raid0(const char *dev, dev_t top_dev,
			  struct topo_user_cfg *cfg)
{
	unsigned long long chunk_bytes;
	unsigned long long chunk_sectors;
	unsigned long long raid_disks;
	char level[64];
	unsigned int shift = 0;
	int err;

	err = read_sysfs_text(top_dev, "md/level", level, sizeof(level));
	if (err)
		return err;
	level[strcspn(level, "\r\n")] = '\0';
	if (strcmp(level, "raid0")) {
		fprintf(stderr, "nds: %s uses unsupported MD level %s\n",
			dev, level);
		return -EOPNOTSUPP;
	}
	err = read_sysfs_u64(top_dev, "md/chunk_size", &chunk_bytes);
	if (err)
		return err;
	err = read_sysfs_u64(top_dev, "md/raid_disks", &raid_disks);
	if (err)
		return err;
	if (raid_disks < 2 || raid_disks > P2P_TOPO_MAX_BDEVS)
		return raid_disks > P2P_TOPO_MAX_BDEVS ? -E2BIG : -EINVAL;
	if (!chunk_bytes || chunk_bytes % NDS_SECTOR_SIZE)
		return -EINVAL;
	chunk_sectors = chunk_bytes / NDS_SECTOR_SIZE;
	if (!chunk_sectors || (chunk_sectors & (chunk_sectors - 1)))
		return -EINVAL;
	while ((1ULL << shift) != chunk_sectors) {
		if (++shift >= 32)
			return -EINVAL;
	}

	strcpy(cfg->name, "raid0");
	cfg->nr_devs = raid_disks;
	cfg->extra[0] = shift;
	cfg->top_dev = top_dev;
	err = discover_raid0_members(top_dev, raid_disks, cfg);
	if (err)
		fprintf(stderr,
			"nds: discover RAID0 members for %s failed: %d\n",
			dev, err);
	return err;
}

static void print_topo_cfg(const struct topo_user_cfg *cfg)
{
	unsigned int i;

	printf("topo: name=%s top_dev=%u:%u nr_devs=%u extra={%llu,%llu}\n",
	       cfg->name, major(cfg->top_dev), minor(cfg->top_dev), cfg->nr_devs,
	       (unsigned long long)cfg->extra[0],
	       (unsigned long long)cfg->extra[1]);
	for (i = 0; i < cfg->nr_devs; i++) {
		const struct topo_user_bdev *bdev = &cfg->bdevs[i];

		printf("  bdev[%u]: dev=%u:%u start_sector=%llu "
		       "size_sector=%llu\n", i, major(bdev->dev_id),
		       minor(bdev->dev_id),
		       (unsigned long long)bdev->start_sector,
		       (unsigned long long)bdev->size_sector);
	}
}

/*
 * Register the topology of the block device at @dev with the kernel.
 * Used once at nds_init for the topo_fd target.
 */
static int nds_add_topo(int dev_fd, const char *dev)
{
	struct topo_user_cfg *cfg = NULL;
	struct stat st;
	size_t cfg_size;
	int (*discover)(const char *, dev_t, struct topo_user_cfg *);
	int err;

	if (stat(dev, &st) < 0)
		return -errno;
	if (!S_ISBLK(st.st_mode))
		return -EINVAL;
	if (path_exists(st.st_rdev, "partition") ||
	    path_exists(st.st_rdev, "device/subsysnqn"))
		discover = discover_nvme;
	else if (path_exists(st.st_rdev, "dm"))
		discover = discover_linear;
	else if (path_exists(st.st_rdev, "md"))
		discover = discover_raid0;
	else
		return -EOPNOTSUPP;

	cfg_size = sizeof(*cfg) +
		   P2P_TOPO_MAX_BDEVS * sizeof(cfg->bdevs[0]);
	cfg = calloc(1, cfg_size);
	if (!cfg) {
		err = -ENOMEM;
		goto out;
	}
	err = discover(dev, st.st_rdev, cfg);
	if (err)
		goto out;
	print_topo_cfg(cfg);
	if (ioctl(dev_fd, IOCTL_ADD_TOPO, cfg) < 0) {
		err = -errno;
		fprintf(stderr, "nds: add topology for %s failed, errno: %d\n",
			dev, err);
	} else {
		err = 0;
	}
out:
	free(cfg);
	return err;
}

/* ------------------------------------------------------------------ */
/* FIEMAP extent packing for RW                                       */
/* ------------------------------------------------------------------ */

static int is_power_of_two(unsigned int value)
{
	return value && !(value & (value - 1));
}

static unsigned int get_fiemap_block_size(int file_fd)
{
	int block_size = 0;

	if (!ioctl(file_fd, FIGETBSZ, &block_size) &&
	    block_size >= (int)NDS_SECTOR_SIZE &&
	    is_power_of_two((unsigned int)block_size))
		return block_size;

	return NDS_DEFAULT_BLOCK_SIZE;
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
	return !(value & (NDS_SECTOR_SIZE - 1));
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
		if (extent.fe_flags & NDS_UNSUPPORTED_FIEMAP_FLAGS) {
			fprintf(stderr,
				"nds: unsupported FIEMAP extent flags 0x%x at extent %u\n",
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
				"nds: unaligned FIEMAP extent logical 0x%llx physical 0x%llx length 0x%llx\n",
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

static int prepare_io_extents(int file_fd, const struct stat *file_stat,
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
		fprintf(stderr, "nds: ioctl FS_IOC_FIEMAP failed, errno: %d\n",
			err);
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
		     (NDS_SECTOR_SIZE - 1)) ||
		    ULONG_MAX - iov[i].addr < iov[i].size ||
		    ULONG_MAX - size < iov[i].size)
			return -EINVAL;
		size += iov[i].size;
	}

	*size_out = size;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public NDS API                                                     */
/* ------------------------------------------------------------------ */

int nds_init(struct nds_init_param *param)
{
	char path[PATH_MAX];
	char bdev[PATH_MAX];
	struct stat st;
	dev_t target;
	char sys[64];
	char link[PATH_MAX];
	const char *base;
	const char *topo_path;
	ssize_t n;
	int32_t topo_fd;
	int ret;

	if (!param)
		return -EINVAL;
	if (param->version != NDS_API_VERSION)
		return -EINVAL;
	if (param->flags)
		return -EOPNOTSUPP;
	if (is_init())
		return -EBUSY;

	/*
	 * Topology is described by an fs_fd array; v1 registers the topology
	 * of the first fd only (single top device).
	 */
	if (!param->desc.fs_fd || param->desc.fs_fd_cnt != 1 ||
	    param->desc.reserved)
		return -EINVAL;
	topo_fd = param->desc.fs_fd[0];

	/* Resolve topo fd to the underlying block device path. */
	snprintf(path, sizeof(path), "/proc/self/fd/%d", topo_fd);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return -errno;
	link[n] = '\0';
	if (stat(link, &st) < 0)
		return -errno;
	if (S_ISBLK(st.st_mode)) {
		topo_path = link;
	} else if (S_ISREG(st.st_mode)) {
		target = st.st_dev;
		snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u",
			 major(target), minor(target));
		n = readlink(sys, link, sizeof(link) - 1);
		if (n < 0)
			return -errno;
		link[n] = '\0';
		base = strrchr(link, '/');
		base = base ? base + 1 : link;
		if (snprintf(bdev, sizeof(bdev), "/dev/%s", base) >=
		    (int)sizeof(bdev))
			return -ENAMETOOLONG;
		topo_path = bdev;
	} else {
		return -EINVAL;
	}

	g_state.book_fd = nds_open_dev();
	if (g_state.book_fd < 0)
		return g_state.book_fd;
	g_state.regs = NULL;

	/* Register topology once for the lifetime of book_fd. */
	ret = nds_add_topo(g_state.book_fd, topo_path);
	if (ret && ret != -EEXIST) {
		close(g_state.book_fd);
		g_state.book_fd = -1;
		return ret;
	}
	return 0;
}

int nds_exit(void)
{
	struct nds_reg_entry *r, *next;

	if (!is_init())
		return -EINVAL;

	for (r = g_state.regs; r; r = next) {
		struct p2p_mem_unregister_param up = {
			.mem_handle = r->handle,
			.reserved = 0,
		};
		next = r->next;
		ioctl(g_state.book_fd, IOCTL_UNREGISTER_MEM, &up);
		free(r);
	}
	g_state.regs = NULL;

	close(g_state.book_fd);
	g_state.book_fd = -1;
	return 0;
}

int nds_register_mem(void *addr, uint64_t size, int flags)
{
	struct p2p_mem_register_param p = { 0 };
	struct nds_reg_entry *r;

	if (!is_init())
		return -EINVAL;
	/* addr may be 0 (stub CMB window starts at VA 0). */
	if (!size || flags)
		return -EINVAL;

	p.addr = (__u64)(uintptr_t)addr;
	p.size = size;
	if (ioctl(g_state.book_fd, IOCTL_REGISTER_MEM, &p) < 0)
		return -errno;

	r = calloc(1, sizeof(*r));
	if (!r) {
		struct p2p_mem_unregister_param up = { .mem_handle = p.mem_handle };
		ioctl(g_state.book_fd, IOCTL_UNREGISTER_MEM, &up);
		return -ENOMEM;
	}
	r->addr = addr;
	r->size = size;
	r->handle = p.mem_handle;
	r->next = g_state.regs;
	g_state.regs = r;

	return 0;
}

int nds_unregister_mem(void *addr)
{
	struct nds_reg_entry *r, **prev;
	struct p2p_mem_unregister_param up;
	int ret = 0;

	if (!is_init())
		return -EINVAL;

	for (prev = &g_state.regs; *prev; prev = &(*prev)->next) {
		if ((*prev)->addr == addr)
			break;
	}
	if (!*prev)
		return -ENOENT;

	r = *prev;
	*prev = r->next;

	up.mem_handle = r->handle;
	up.reserved = 0;
	if (ioctl(g_state.book_fd, IOCTL_UNREGISTER_MEM, &up) < 0)
		ret = -errno;
	free(r);
	return ret;
}

int nds_io_new_ctx(const struct nds_io_ctx_param *param, struct nds_io_ctx **ctx)
{
	struct nds_io_ctx *c;
	int fd;

	if (!is_init() || !param || !ctx)
		return -EINVAL;
	if (param->max_io_nr == 0 || param->flags || param->reserved)
		return -EINVAL;

	fd = nds_open_dev();
	if (fd < 0)
		return fd;

	c = calloc(1, sizeof(*c));
	if (!c) {
		close(fd);
		return -ENOMEM;
	}
	c->p2p_fd = fd;
	c->max_io_nr = param->max_io_nr;
	*ctx = c;
	return 0;
}

int nds_io_destroy_ctx(struct nds_io_ctx *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;

	if (ctx->inflight) {
		ret = ioctl(ctx->p2p_fd, IOCTL_DRAIN_IO);
		if (ret < 0)
			fprintf(stderr, "nds: destroy_ctx drain warning %d\n",
				-errno);
	}
	ret = close(ctx->p2p_fd);
	free(ctx);
	return ret;
}

static int resolve_obj_to_fd(struct nds_io_obj *obj, int *fd_out)
{
	if (!obj)
		return -EINVAL;
	if (obj->type == NDS_IO_OBJ_FD) {
		if (obj->fd < 0)
			return -EINVAL;
		*fd_out = obj->fd;
		return 0;
	}
	if (obj->type == NDS_IO_OBJ_ID)
		return -EOPNOTSUPP;
	return -EINVAL;
}

static int nds_iocb_to_io_param(struct nds_io_cb *cb, int file_fd,
				struct p2p_io_param **io_out,
				struct p2p_iov *iov)
{
	struct nds_reg_entry *r;
	struct p2p_io_param *io;
	struct fiemap *exts = NULL;
	struct stat file_stat;
	unsigned long io_size;
	unsigned long long total_size = 0;
	size_t request_size;
	unsigned int ext_num = 0;
	uint64_t buf_addr;
	int err;

	if (cb->opcode > NDS_IO_OP_PWRITEV)
		return -EOPNOTSUPP;
	if (cb->opcode == NDS_IO_OP_PREADV || cb->opcode == NDS_IO_OP_PWRITEV)
		return -EOPNOTSUPP; /* TODO */
	if (cb->rw_flags & ~NDS_IO_F_MASK)
		return -EOPNOTSUPP;

	iov->addr = cb->buf_addr;
	iov->size = cb->buf_len;
	err = get_iov_size(iov, 1, &io_size);
	if (err)
		return err;
	if (ULONG_MAX - cb->offset < io_size)
		return -EOVERFLOW;

	if (fstat(file_fd, &file_stat) < 0) {
		err = -errno;
		fprintf(stderr, "nds: fstat fd %d failed, errno: %d\n",
			file_fd, err);
		return err;
	}

	if ((cb->opcode == NDS_IO_OP_PWRITE) && !S_ISBLK(file_stat.st_mode)) {
		return S_ISREG(file_stat.st_mode) ? -EOPNOTSUPP : -EINVAL;
	}

	err = prepare_io_extents(file_fd, &file_stat, cb->offset, io_size,
				 &exts, &ext_num, &total_size);
	if (err) {
		fprintf(stderr, "nds: prepare extents failed, errno: %d\n", err);
		return err;
	}
	if (!ext_num || total_size < io_size) {
		fprintf(stderr, "nds: extent size %llu < IOV size %lu\n",
			total_size, io_size);
		free(exts);
		return -ENODATA;
	}

	if (__builtin_mul_overflow((size_t)ext_num, sizeof(io->extents[0]),
				   &request_size) ||
	    __builtin_add_overflow(request_size, sizeof(*io), &request_size)) {
		free(exts);
		return -E2BIG;
	}
	io = calloc(1, request_size);
	if (!io) {
		free(exts);
		return -ENOMEM;
	}

	io->op = (cb->opcode == NDS_IO_OP_PWRITE) ? P2P_IO_WRITE : P2P_IO_READ;
	io->file_fd = file_fd;
	io->user_data = cb->user_data;
	io->iov = iov;
	io->iov_nr = 1;
	io->ext_nr = ext_num;
	memcpy(io->extents, exts->fm_extents, ext_num * sizeof(io->extents[0]));
	free(exts);

	if (cb->rw_flags & NDS_IO_F_REGISTERED_MEM) {
		if (cb->host_pid) {
			free(io);
			return -EINVAL;
		}
		buf_addr = cb->buf_addr;
		for (r = g_state.regs; r; r = r->next) {
			if (buf_addr >= (uint64_t)(uintptr_t)r->addr &&
			    buf_addr < (uint64_t)(uintptr_t)r->addr + r->size)
				break;
		}
		if (!r) {
			free(io);
			return -EINVAL;
		}
		io->flags = P2P_IO_F_REGISTERED_MEM;
		io->host_pid = 0;
		io->mem_handle = r->handle;
	} else {
		if (cb->host_pid < 0) {
			free(io);
			return -EINVAL;
		}
		io->flags = 0;
		io->host_pid = cb->host_pid ? cb->host_pid : getpid();
		io->mem_handle = 0;
	}

	*io_out = io;
	return 0;
}

int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr, struct nds_io_cb *iocb)
{
	unsigned int i;
	int ret;

	if (!ctx || !iocb)
		return -EINVAL;
	if (nr == 0)
		return 0;

	for (i = 0; i < nr; i++) {
		struct p2p_io_param *io = NULL;
		struct p2p_iov iov;
		struct nds_io_cb *cb = &iocb[i];
		int file_fd;

		if (ctx->inflight >= ctx->max_io_nr)
			return i ? (int)i : -EAGAIN;

		ret = resolve_obj_to_fd(&cb->obj, &file_fd);
		if (ret) {
			fprintf(stderr, "nds: resolve_obj failed %d\n", ret);
			return i ? (int)i : ret;
		}

		ret = nds_iocb_to_io_param(cb, file_fd, &io, &iov);
		if (ret) {
			fprintf(stderr, "nds: translate iocb failed %d\n", ret);
			return i ? (int)i : ret;
		}

		ret = ioctl(ctx->p2p_fd, IOCTL_RW_FILE, io);
		if (ret < 0) {
			ret = -errno;
			fprintf(stderr, "nds: rw ioctl failed %d\n", ret);
			free(io);
			return i ? (int)i : ret;
		}
		free(io);
		ctx->inflight++;
	}
	return (int)i;
}

int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout)
{
	struct p2p_getevents_param param = { 0 };
	struct p2p_io_event *raw = NULL;
	int ret;
	int i;

	if (!ctx || !events)
		return -EINVAL;
	if (nr == 0)
		return 0;
	if (min_nr > nr)
		return -EINVAL;

	raw = calloc(nr, sizeof(*raw));
	if (!raw)
		return -ENOMEM;

	param.min_nr = min_nr;
	param.max_nr = nr;
	if (timeout) {
		param.timeout_ns = (__s64)timeout->tv_sec * 1000000000LL +
				   timeout->tv_nsec;
	} else {
		param.timeout_ns = -1;
	}
	param.events = raw;

	ret = ioctl(ctx->p2p_fd, IOCTL_GET_IO_EVENTS, &param);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "nds: getevents ioctl failed %d\n", ret);
		free(raw);
		return ret;
	}
	for (i = 0; i < ret; i++) {
		memset(&events[i], 0, sizeof(events[i]));
		events[i].user_data = raw[i].user_data;
		events[i].res = raw[i].res;
		events[i].opcode = raw[i].op;
	}
	free(raw);
	if (ret > 0)
		ctx->inflight -= (unsigned int)ret;
	return ret;
}
