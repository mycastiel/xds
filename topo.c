// SPDX-License-Identifier: GPL-2.0
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/kdev_t.h>
#include <linux/namei.h>
#include <linux/nvme_ioctl.h>
#include <linux/overflow.h>
#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "topo.h"

#define P2P_SYSFS_BDEV_PATH_MAX 48

static RADIX_TREE(topo_tree, GFP_ATOMIC);
static DEFINE_SPINLOCK(topo_lock);
static struct workqueue_struct *topo_release_wq;

static bool topo_sysfs_path_exists(dev_t devt, const char *suffix)
{
	char name[P2P_SYSFS_BDEV_PATH_MAX];
	struct path path;
	int len;
	int err;

	len = snprintf(name, sizeof(name), "/sys/dev/block/%u:%u/%s",
		       MAJOR(devt), MINOR(devt), suffix);
	if (WARN_ON_ONCE(len >= sizeof(name)))
		return false;

	err = kern_path(name, LOOKUP_FOLLOW, &path);
	if (err)
		return false;
	path_put(&path);
	return true;
}

static int topo_validate_nvme_bdev(struct block_device *bdev)
{
	dev_t devt = bdev->bd_dev;

	if (bdev_is_partition(bdev))
		return -EOPNOTSUPP;
	if (!topo_sysfs_path_exists(devt, "device/subsysnqn"))
		return -EOPNOTSUPP;
	if (topo_sysfs_path_exists(devt, "device/iopolicy"))
		return -EOPNOTSUPP;
	return 0;
}

static int topo_get_nsid(struct block_device *bdev, u32 *nsid)
{
	const struct block_device_operations *fops = bdev->bd_disk->fops;
	int ret;

	if (!fops || !fops->ioctl)
		return -EOPNOTSUPP;

	ret = fops->ioctl(bdev, BLK_OPEN_READ, NVME_IOCTL_ID, 0);
	if (ret < 0)
		return ret;
	if (!ret)
		return -EOPNOTSUPP;

	*nsid = ret;
	return 0;
}

static int linear_map_sector(struct topo *topo, u64 from_sector,
			     u32 in_nr_sectors, struct topo_bdev **to_bdev,
			     u64 *to_sector, u32 *out_nr_sectors)
{
	u64 logical_start = 0;
	u64 offset;
	u64 mapped;
	u64 available;
	u32 i;

	if (!in_nr_sectors || from_sector >= topo->size_sector)
		return -EINVAL;

	for (i = 0; i < topo->nr_bdevs; i++) {
		struct topo_bdev *bdev = &topo->bdevs[i];

		if (from_sector - logical_start >= bdev->size_sector) {
			logical_start += bdev->size_sector;
			continue;
		}

		offset = from_sector - logical_start;
		if (check_add_overflow(bdev->start_sector, offset, &mapped))
			return -EOVERFLOW;
		available = bdev->size_sector - offset;
		*to_bdev = bdev;
		*to_sector = mapped;
		*out_nr_sectors = min_t(u64, in_nr_sectors, available);
		return 0;
	}

	return -EINVAL;
}

static int raid0_map_sector(struct topo *topo, u64 from_sector,
			    u32 in_nr_sectors, struct topo_bdev **to_bdev,
			    u64 *to_sector, u32 *out_nr_sectors)
{
	u64 chunk_sectors = 1ULL << topo->priv.chunk_sectors_shift;
	u64 chunk_index;
	u64 offset_in_chunk;
	u64 member_chunk;
	u64 member_offset;
	u64 mapped;
	u32 member_index;

	if (!in_nr_sectors || from_sector >= topo->size_sector)
		return -EINVAL;

	chunk_index = from_sector >> topo->priv.chunk_sectors_shift;
	offset_in_chunk = from_sector & (chunk_sectors - 1);
	member_index = chunk_index % topo->nr_bdevs;
	member_chunk = div_u64(chunk_index, topo->nr_bdevs);
	if (check_mul_overflow(member_chunk, chunk_sectors, &member_offset) ||
	    check_add_overflow(member_offset, offset_in_chunk, &member_offset) ||
	    check_add_overflow(topo->bdevs[member_index].start_sector,
			       member_offset, &mapped))
		return -EOVERFLOW;

	*to_bdev = &topo->bdevs[member_index];
	*to_sector = mapped;
	*out_nr_sectors = min_t(u64, in_nr_sectors,
				    chunk_sectors - offset_in_chunk);
	return 0;
}

static const struct topo_ops linear_ops = {
	.map_sector = linear_map_sector,
};

static const struct topo_ops raid0_ops = {
	.map_sector = raid0_map_sector,
};

static void topo_release_handles(struct topo *topo)
{
	u32 i;

	if (topo->bdevs) {
		for (i = 0; i < topo->nr_bdevs; i++) {
			if (topo->bdevs[i].handle)
				bdev_release(topo->bdevs[i].handle);
		}
	}
	if (topo->top_handle)
		bdev_release(topo->top_handle);
}

static void topo_destroy_sync(struct topo *topo)
{
	if (!topo)
		return;
	topo_release_handles(topo);
	kfree(topo->bdevs);
	kfree(topo);
}

static void topo_release_work(struct work_struct *work)
{
	struct rcu_work *rcu_work = to_rcu_work(work);
	struct topo *topo = container_of(rcu_work, struct topo, rcu_work);

	topo_destroy_sync(topo);
}

static void topo_ref_release(struct kref *ref)
{
	struct topo *topo = container_of(ref, struct topo, refcnt);

	if (WARN_ON_ONCE(!queue_rcu_work(topo_release_wq, &topo->rcu_work)))
		return;
}

void topo_put(struct topo *topo)
{
	kref_put(&topo->refcnt, topo_ref_release);
}

static int topo_validate_linear(struct topo *topo,
				const struct topo_user_cfg *cfg)
{
	u64 size = 0;
	u32 i;

	if (cfg->extra[0] || cfg->extra[1])
		return -EINVAL;

	for (i = 0; i < topo->nr_bdevs; i++) {
		if (check_add_overflow(size, topo->bdevs[i].size_sector, &size))
			return -EINVAL;
	}
	if (size != bdev_nr_sectors(topo->top_handle->bdev))
		return -EINVAL;

	topo->size_sector = size;
	topo->ops = &linear_ops;
	return 0;
}

static int topo_validate_raid0(struct topo *topo,
			       const struct topo_user_cfg *cfg)
{
	u64 chunk_sectors;
	u64 size;
	u32 i;

	if (topo->nr_bdevs < 2 || cfg->extra[1] || cfg->extra[0] >= 32)
		return -EINVAL;

	chunk_sectors = 1ULL << cfg->extra[0];
	for (i = 0; i < topo->nr_bdevs; i++) {
		if (topo->bdevs[i].start_sector != topo->bdevs[0].start_sector ||
		    topo->bdevs[i].size_sector != topo->bdevs[0].size_sector ||
		    topo->bdevs[i].size_sector % chunk_sectors)
			return -EINVAL;
	}
	if (check_mul_overflow(topo->bdevs[0].size_sector,
			       (u64)topo->nr_bdevs, &size))
		return -EINVAL;
	if (size != bdev_nr_sectors(topo->top_handle->bdev))
		return -EINVAL;

	topo->size_sector = size;
	topo->priv.chunk_sectors_shift = cfg->extra[0];
	topo->ops = &raid0_ops;
	return 0;
}

static int topo_validate_cfg(const struct topo_user_cfg *cfg,
			     const struct topo *topo)
{
	u32 i;
	u32 j;

	if (!memchr(cfg->name, '\0', sizeof(cfg->name)) ||
	    (strcmp(cfg->name, "linear") && strcmp(cfg->name, "raid0")))
		return -EINVAL;
	if (!cfg->top_dev || !topo->top_dev || !cfg->nr_devs ||
	    cfg->nr_devs > P2P_TOPO_MAX_BDEVS)
		return -EINVAL;

	for (i = 0; i < cfg->nr_devs; i++) {
		dev_t id = new_decode_dev(cfg->bdevs[i].dev_id);

		if (!cfg->bdevs[i].dev_id || !id || cfg->bdevs[i].reserved ||
		    !cfg->bdevs[i].size_sector || id == topo->top_dev)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			if (id == new_decode_dev(cfg->bdevs[j].dev_id))
				return -EINVAL;
		}
	}
	return 0;
}

static struct topo *topo_alloc_candidate(const struct topo_user_cfg *cfg)
{
	struct topo *topo;
	u32 i;
	int err;

	topo = kzalloc(sizeof(*topo), GFP_KERNEL);
	if (!topo)
		return ERR_PTR(-ENOMEM);

	topo->top_dev = new_decode_dev(cfg->top_dev);
	topo->nr_bdevs = cfg->nr_devs;
	err = topo_validate_cfg(cfg, topo);
	if (err)
		goto free_topo;

	topo->bdevs = kcalloc(topo->nr_bdevs, sizeof(*topo->bdevs),
			      GFP_KERNEL);
	if (!topo->bdevs) {
		err = -ENOMEM;
		goto free_topo;
	}
	strscpy(topo->name, cfg->name, sizeof(topo->name));
	INIT_RCU_WORK(&topo->rcu_work, topo_release_work);

	topo->top_handle = bdev_open_by_dev(topo->top_dev, BLK_OPEN_READ,
					     NULL, NULL);
	if (IS_ERR(topo->top_handle)) {
		err = PTR_ERR(topo->top_handle);
		topo->top_handle = NULL;
		goto free_topo;
	}
	if (topo->top_handle->bdev->bd_dev != topo->top_dev) {
		err = -ENODEV;
		goto free_topo;
	}
	if ((!strcmp(topo->name, "linear") &&
	     !topo_sysfs_path_exists(topo->top_dev, "dm")) ||
	    (!strcmp(topo->name, "raid0") &&
	     !topo_sysfs_path_exists(topo->top_dev, "md"))) {
		err = -EOPNOTSUPP;
		goto free_topo;
	}

	for (i = 0; i < topo->nr_bdevs; i++) {
		struct topo_bdev *bdev = &topo->bdevs[i];
		u64 end_sector;

		bdev->id = new_decode_dev(cfg->bdevs[i].dev_id);
		bdev->size_sector = cfg->bdevs[i].size_sector;
		bdev->start_sector = cfg->bdevs[i].start_sector;
		bdev->handle = bdev_open_by_dev(bdev->id, BLK_OPEN_READ,
						NULL, NULL);
		if (IS_ERR(bdev->handle)) {
			err = PTR_ERR(bdev->handle);
			bdev->handle = NULL;
			goto free_topo;
		}
		if (bdev->handle->bdev->bd_dev != bdev->id) {
			err = -ENODEV;
			goto free_topo;
		}
		err = topo_validate_nvme_bdev(bdev->handle->bdev);
		if (err)
			goto free_topo;
		if (check_add_overflow(bdev->start_sector, bdev->size_sector,
				       &end_sector) ||
		    end_sector > bdev_nr_sectors(bdev->handle->bdev)) {
			err = -EINVAL;
			goto free_topo;
		}
		err = topo_get_nsid(bdev->handle->bdev, &bdev->nsid);
		if (err)
			goto free_topo;
	}

	if (!strcmp(topo->name, "linear"))
		err = topo_validate_linear(topo, cfg);
	else
		err = topo_validate_raid0(topo, cfg);
	if (err)
		goto free_topo;

	kref_init(&topo->refcnt);
	kref_init(&topo->pin_refcnt);
	return topo;

free_topo:
	topo_destroy_sync(topo);
	return ERR_PTR(err);
}

struct topo *topo_add(const struct topo_user_cfg *cfg,
		      struct shared_topo_list *list)
{
	struct shared_topo *shared;
	struct topo *candidate;
	struct topo *installed;
	int err;

	if (!cfg || !list)
		return ERR_PTR(-EINVAL);

	shared = kzalloc(sizeof(*shared), GFP_KERNEL);
	if (!shared)
		return ERR_PTR(-ENOMEM);
	candidate = topo_alloc_candidate(cfg);
	if (IS_ERR(candidate)) {
		err = PTR_ERR(candidate);
		goto free_shared;
	}

	err = radix_tree_preload(GFP_KERNEL);
	if (err)
		goto free_candidate;

	spin_lock(&topo_lock);
	installed = radix_tree_lookup(&topo_tree, candidate->top_dev);
	if (installed) {
		kref_get(&installed->pin_refcnt);
	} else {
		err = radix_tree_insert(&topo_tree, candidate->top_dev, candidate);
		if (!err)
			installed = candidate;
	}
	spin_unlock(&topo_lock);
	radix_tree_preload_end();
	if (err)
		goto free_candidate;

	shared->topo = installed;
	spin_lock(&list->lock);
	list_add_tail(&shared->list, &list->head);
	spin_unlock(&list->lock);

	if (installed != candidate)
		topo_destroy_sync(candidate);
	return installed;

free_candidate:
	topo_destroy_sync(candidate);
free_shared:
	kfree(shared);
	return ERR_PTR(err);
}

struct topo *topo_get(struct block_device *top_bdev)
{
	struct topo *topo;

	rcu_read_lock();
	topo = radix_tree_lookup(&topo_tree, top_bdev->bd_dev);
	if (!topo) {
		rcu_read_unlock();
		return NULL;
	}
	if (!kref_get_unless_zero(&topo->refcnt)) {
		rcu_read_unlock();
		return ERR_PTR(-ESTALE);
	}
	if (unlikely(READ_ONCE(topo->deleting))) {
		rcu_read_unlock();
		topo_put(topo);
		return ERR_PTR(-ESTALE);
	}
	rcu_read_unlock();
	return topo;
}

static void topo_pin_release(struct kref *ref)
{
	struct topo *topo = container_of(ref, struct topo, pin_refcnt);
	void *removed;

	WRITE_ONCE(topo->deleting, true);
	removed = radix_tree_delete(&topo_tree, topo->top_dev);
	WARN_ON_ONCE(removed != topo);
	topo_put(topo);
	spin_unlock(&topo_lock);
}

void topo_del(struct topo *topo)
{
	if (WARN_ON_ONCE(!topo))
		return;
	kref_put_lock(&topo->pin_refcnt, topo_pin_release, &topo_lock);
}

int topo_init(void)
{
	topo_release_wq = alloc_workqueue("p2p_topo_release",
					  WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!topo_release_wq)
		return -ENOMEM;
	return 0;
}

void topo_exit(void)
{
	WARN_ON_ONCE(!radix_tree_empty(&topo_tree));
	rcu_barrier();
	destroy_workqueue(topo_release_wq);
	topo_release_wq = NULL;
}
