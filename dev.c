// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "p2p: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/vmalloc.h>
#include <linux/notifier.h>
#include <linux/tracepoint.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#ifdef CALC_CRC32
#include <linux/crc32.h>
#endif

#include <linux/nvme.h>
#include <linux/blk_types.h>
#include <linux/file.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>

#include <trace/events/block.h>

#include "p2p_dev_uapi.h"
#include "p2p_mem_query.h"
#include "compat.h"
#include "debugfs.h"
#include "dev.h"
#include "topo.h"

#define P2P_NVME_MAX_RW_SECTORS ((1U << 20) >> SECTOR_SHIFT)
#define P2P_MAX_IOV 65536U
#define P2P_MAX_EXTENTS 1048576U
#define P2P_MIN_PAGE_SIZE (64U << 10)

struct p2p_pa_iov {
	u64 addr;
	u64 len;
};

struct p2p_iov_iter {
	const struct p2p_pa_iov *iov;
	unsigned int nr_segs;
	u64 iov_offset;
	u64 count;
};

struct p2p_iov_map {
	u64 aligned_addr;
	u64 aligned_size;
	u64 *pa_list;
	u32 page_size;
	u32 pa_num;
};

struct p2p_pinned_pa {
	struct devmm_svm_process_id process_id;
	struct p2p_iov_map *maps;
	unsigned int pinned_map_nr;
	u64 *pa_list;
};

struct p2p_io_context {
	struct file *file;
	struct p2p_pa_iov *pa_iov;
	unsigned int pa_iov_nr;
	struct p2p_pinned_pa *pinned_pa;
	u64 data_size;
	struct completion io_done;
	atomic_t io_ref;
	int io_err;
	int issue_err;
	struct list_head io_list;
};

struct p2p_batch {
	struct list_head io_list;
	unsigned int io_cnt;
	spinlock_t io_lock;
	struct shared_topo_list shared_topos;
};

static void p2p_tp_hook_exit(void);

static dev_t dev;
static struct class *dev_class;
static struct cdev p2p_cdev;

static struct tracepoint *p2p_tp;
static DEFINE_MUTEX(p2p_tp_lock);
static struct module *p2p_tp_mod;

static int p2p_open(struct inode *inode, struct file *file);
static int p2p_release(struct inode *inode, struct file *file);
static long p2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int p2p_drain_io(struct p2p_batch *batch);

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = p2p_open,
	.release = p2p_release,
	.unlocked_ioctl = p2p_ioctl,
};

static int p2p_open(struct inode *inode, struct file *file)
{
	struct p2p_batch *batch;

	batch = kzalloc(sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return -ENOMEM;

	spin_lock_init(&batch->io_lock);
	INIT_LIST_HEAD(&batch->io_list);
	spin_lock_init(&batch->shared_topos.lock);
	INIT_LIST_HEAD(&batch->shared_topos.head);
	batch->io_cnt = 0;
	file->private_data = batch;

	return 0;
}

static int p2p_release(struct inode *inode, struct file *file)
{
	struct p2p_batch *batch = file->private_data;
	struct shared_topo *shared, *next;
	LIST_HEAD(topos);

	if (!list_empty(&batch->io_list) || batch->io_cnt > 0)
		p2p_drain_io(batch);

	spin_lock(&batch->shared_topos.lock);
	list_splice_init(&batch->shared_topos.head, &topos);
	spin_unlock(&batch->shared_topos.lock);
	list_for_each_entry_safe(shared, next, &topos, list) {
		list_del(&shared->list);
		topo_del(shared->topo);
		kfree(shared);
	}

	kfree(batch);

	file->private_data = NULL;

	return 0;
}

static void init_process_id(int host_pid, struct devmm_svm_process_id *process_id)
{
	rcu_read_lock();
	process_id->host_pid = pid_nr(find_vpid(host_pid));
	rcu_read_unlock();
}

static void p2p_put_pinned_pa(struct p2p_pinned_pa *pinned_pa)
{
	unsigned int i;

	if (!pinned_pa)
		return;

	for (i = 0; i < pinned_pa->pinned_map_nr; i++) {
		struct p2p_iov_map *map = &pinned_pa->maps[i];

		devmm_put_mem_pa_list(&pinned_pa->process_id, map->aligned_addr,
				      map->aligned_size, map->pa_list, map->pa_num);
	}

	kvfree(pinned_pa->pa_list);
	kvfree(pinned_pa->maps);
	kfree(pinned_pa);
}

#ifdef DUMP_CONTENT
static void dump_pa_content(u64 pa, u64 size)
{
	unsigned int data_len = min_t(u64, size, 16U << 10);
	void *addr;

	addr = ioremap(pa, data_len);
	if (!addr) {
		return;
	}

	print_hex_dump(KERN_INFO, "PA 0x%llx content: ", pa, DUMP_PREFIX_ADDRESS,
		       16, 1, addr, data_len, false);

	iounmap(addr);
}
#else
static void dump_pa_content(u64 pa, u64 size)
{
}
#endif

static void p2p_iov_iter_init(struct p2p_iov_iter *iter,
			      const struct p2p_pa_iov *iov,
			      unsigned int nr_segs, u64 count)
{
	*iter = (struct p2p_iov_iter) {
		.iov = iov,
		.nr_segs = nr_segs,
		.count = count,
	};
}

static inline u64 p2p_iov_iter_count(const struct p2p_iov_iter *iter)
{
	return iter->count;
}

static inline u64 p2p_iov_iter_addr(const struct p2p_iov_iter *iter)
{
	return iter->iov->addr + iter->iov_offset;
}

static u32 p2p_iov_iter_single_seg_sectors(const struct p2p_iov_iter *iter)
{
	if (!iter->count)
		return 0;
	if (WARN_ON_ONCE(iter->iov_offset >= iter->iov->len))
		return 0;
	return (iter->iov->len - iter->iov_offset) >> SECTOR_SHIFT;
}

static void p2p_iov_iter_advance(struct p2p_iov_iter *iter, u64 bytes)
{
	if (WARN_ON_ONCE(bytes > iter->count)) {
		iter->count = 0;
		return;
	}

	iter->count -= bytes;
	iter->iov_offset += bytes;

	if (iter->iov_offset < iter->iov->len)
		return;

	iter->iov++;
	iter->nr_segs--;
	iter->iov_offset = 0;
}

static int get_pa_iov(int host_pid, const struct p2p_iov *iov,
		      unsigned int iov_nr, struct p2p_pa_iov **pa_iov,
		      unsigned int *pa_iov_nr, struct p2p_pinned_pa **pinned_pa_out)
{
	struct p2p_pinned_pa *pinned_pa;
	struct p2p_pa_iov *new_pa_iov;
	struct p2p_iov_map *maps;
	unsigned int new_pa_iov_nr;
	unsigned int new_pa_iov_idx;
	unsigned int i;
	int err;

	pinned_pa = kzalloc(sizeof(*pinned_pa), GFP_KERNEL);
	if (!pinned_pa)
		return -ENOMEM;
	init_process_id(host_pid, &pinned_pa->process_id);
	pinned_pa->maps = kvcalloc(iov_nr, sizeof(*pinned_pa->maps), GFP_KERNEL);
	if (!pinned_pa->maps) {
		err = -ENOMEM;
		goto put_pinned_pa;
	}
	maps = pinned_pa->maps;

	new_pa_iov_nr = 0;
	for (i = 0; i < iov_nr; i++) {
		u64 range_size;
		u64 pa_num;
		u64 offset;
		int page_size;

		page_size = devmm_get_mem_page_size(&pinned_pa->process_id,
						    iov[i].addr, iov[i].size);
		if (page_size < P2P_MIN_PAGE_SIZE ||
		    !is_power_of_2(page_size)) {
			pr_err("invalid page size %d for addr 0x%lx, iov %u\n",
			       page_size, iov[i].addr, i);
			err = -EINVAL;
			goto put_pinned_pa;
		}

		offset = iov[i].addr & (page_size - 1);
		range_size = offset + iov[i].size;
		pa_num = DIV_ROUND_UP_ULL(range_size, page_size);
		if (!pa_num || pa_num > UINT_MAX || pa_num > UINT_MAX - new_pa_iov_nr) {
			err = -E2BIG;
			goto put_pinned_pa;
		}

		maps[i].aligned_addr = iov[i].addr - offset;
		maps[i].page_size = page_size;
		maps[i].pa_num = (u32)pa_num;
		maps[i].aligned_size = pa_num * page_size;
		new_pa_iov_nr += maps[i].pa_num;
	}

	pinned_pa->pa_list = kvmalloc_array(new_pa_iov_nr, sizeof(*pinned_pa->pa_list), GFP_KERNEL);
	if (!pinned_pa->pa_list) {
		err = -ENOMEM;
		goto put_pinned_pa;
	}

	new_pa_iov = kvmalloc_array(new_pa_iov_nr, sizeof(*new_pa_iov), GFP_KERNEL);
	if (!new_pa_iov) {
		err = -ENOMEM;
		goto put_pinned_pa;
	}

	new_pa_iov_idx = 0;
	for (i = 0; i < iov_nr; i++) {
		u64 remaining = iov[i].size;
		u64 offset = iov[i].addr - maps[i].aligned_addr;
		unsigned int j;

		maps[i].pa_list = pinned_pa->pa_list + new_pa_iov_idx;
		err = devmm_get_mem_pa_list(&pinned_pa->process_id,
					    maps[i].aligned_addr, maps[i].aligned_size,
					    maps[i].pa_list, maps[i].pa_num);
		if (err) {
			pr_err("PA list addr 0x%llx size 0x%llx num %u iov %u err %d\n",
			       maps[i].aligned_addr, maps[i].aligned_size,
			       maps[i].pa_num, i, err);
			goto free_pa_iov;
		}
		pinned_pa->pinned_map_nr++;

		for (j = 0; j < maps[i].pa_num; j++) {
			u64 len = min_t(u64, remaining, maps[i].page_size - offset);

			new_pa_iov[new_pa_iov_idx].addr = maps[i].pa_list[j] + offset;
			new_pa_iov[new_pa_iov_idx].len = len;
			if ((new_pa_iov[new_pa_iov_idx].addr | len) & (SECTOR_SIZE - 1)) {
				pr_err("unaligned PA IOV addr 0x%llx, len 0x%llx, iov %u\n",
				       new_pa_iov[new_pa_iov_idx].addr, len, i);
				err = -EINVAL;
				break;
			}
			new_pa_iov_idx++;
			remaining -= len;
			offset = 0;
		}

		if (err)
			goto free_pa_iov;
		if (remaining) {
			pr_err("PA list leaves 0x%llx bytes for iov %u\n",
			       remaining, i);
			err = -EINVAL;
			goto free_pa_iov;
		}
	}

	*pa_iov = new_pa_iov;
	*pa_iov_nr = new_pa_iov_nr;
	*pinned_pa_out = pinned_pa;
	return 0;

free_pa_iov:
	kvfree(new_pa_iov);
put_pinned_pa:
	p2p_put_pinned_pa(pinned_pa);
	return err;
}

static void free_io_ctx(struct p2p_io_context *io_ctx)
{
	p2p_put_pinned_pa(io_ctx->pinned_pa);
	kvfree(io_ctx->pa_iov);
	fput(io_ctx->file);
	kfree(io_ctx);
}

static inline unsigned int p2p_queue_max_sectors(struct block_device *bdev)
{
	unsigned int max_sectors;

	max_sectors = queue_max_sectors(bdev_get_queue(bdev));

	return min(max_sectors, P2P_NVME_MAX_RW_SECTORS);
}

static struct p2p_io_context *new_io_ctx(struct file *file,
					 struct p2p_pa_iov *pa_iov,
					 unsigned int pa_iov_nr,
					 struct p2p_pinned_pa *pinned_pa, u64 data_size)
{
	struct p2p_io_context *io_ctx;

	io_ctx = kzalloc(sizeof(*io_ctx), GFP_KERNEL);
	if (!io_ctx)
		return ERR_PTR(-ENOMEM);

	io_ctx->file = file;
	io_ctx->pa_iov = pa_iov;
	io_ctx->pa_iov_nr = pa_iov_nr;
	io_ctx->pinned_pa = pinned_pa;
	io_ctx->data_size = data_size;

	atomic_set(&io_ctx->io_ref, 1);
	INIT_LIST_HEAD(&io_ctx->io_list);
	init_completion(&io_ctx->io_done);

	return io_ctx;
}

static void p2p_hook_nvme_setup_cmd(void *ignore, struct request *rq,
				    struct nvme_command *cmd)
{
	if (rq->end_io != p2p_end_read_io)
		return;

	cmd->rw.flags = NVME_CMD_SGL_METABUF;
}

static bool is_nvme_setup_cmd_tp(struct tracepoint *tp)
{
	return !strcmp(tp->name, "nvme_setup_cmd");
}

static int p2p_register_tp_hook(struct tracepoint *tp)
{
	int err;

	err = tracepoint_probe_register(tp, p2p_hook_nvme_setup_cmd, NULL);
	if (!err) {
		p2p_tp = tp;
		pr_info("registered nvme_setup_cmd tp hook\n");
		return 0;
	}

	pr_warn("register tp hook err %d\n", err);
	return err;
}

static void p2p_find_builtin_tp(struct tracepoint *tp, void *priv)
{
	struct tracepoint **found = priv;

	if (!*found && is_nvme_setup_cmd_tp(tp))
		*found = tp;
}

#ifdef CONFIG_MODULES
static void p2p_register_tp_from_module(struct tp_module *module_tp)
{
	struct module *mod = module_tp->mod;
	unsigned int i;
	int err;

	for (i = 0; i < mod->num_tracepoints; i++) {
		struct tracepoint *tp;

		tp = tracepoint_ptr_deref(&mod->tracepoints_ptrs[i]);
		if (!is_nvme_setup_cmd_tp(tp))
			continue;

		if (!try_module_get(mod)) {
			pr_warn("failed to pin module\n");
			return;
		}

		err = p2p_register_tp_hook(tp);
		if (!err) {
			p2p_tp_mod = mod;
			return;
		}
		module_put(mod);
		return;
	}
}
#endif

static int p2p_tp_module_notify(struct notifier_block *nb, unsigned long val,
				void *data)
{
#ifdef CONFIG_MODULES
	struct tp_module *tp_mod = data;

	mutex_lock(&p2p_tp_lock);
	switch (val) {
	case MODULE_STATE_COMING:
		if (!p2p_tp)
			p2p_register_tp_from_module(tp_mod);
		break;
	default:
		break;
	}
	mutex_unlock(&p2p_tp_lock);
#endif

	return NOTIFY_OK;
}

static struct notifier_block p2p_tp_module_nb = {
	.notifier_call = p2p_tp_module_notify,
};

static int p2p_tp_hook_init(void)
{
	struct tracepoint *tp = NULL;
	int err;

	for_each_kernel_tracepoint(p2p_find_builtin_tp, &tp);
	if (tp)
		return p2p_register_tp_hook(tp);

	err = register_tracepoint_module_notifier(&p2p_tp_module_nb);
	if (err) {
		pr_err("register tp module notifier err %d\n", err);
		return err;
	}

	mutex_lock(&p2p_tp_lock);
	err = p2p_tp ? 0 : -ENOENT;
	mutex_unlock(&p2p_tp_lock);

	unregister_tracepoint_module_notifier(&p2p_tp_module_nb);

	if (err) {
		pr_err("tp not found\n");
		/*
		 * Notifier registration can synchronously invoke COMING
		 * callbacks, so use the shared exit path to undo any hook or
		 * module pin installed before this validation failed.
		 */
		p2p_tp_hook_exit();
	}

	return err;
}

static void p2p_tp_hook_exit(void)
{
	int err;

	if (!p2p_tp)
		return;

	err = tracepoint_probe_unregister(p2p_tp, p2p_hook_nvme_setup_cmd, NULL);
	if (err)
		pr_warn("unregister tp hook err %d\n", err);

	tracepoint_synchronize_unregister();

	module_put(p2p_tp_mod);
}

void p2p_complete_read_io(struct request *req, blk_status_t status)
{
	struct p2p_io_context *io_ctx = req->end_io_data;

	p2p_stats_io_complete(status);
	if (status)
		cmpxchg(&io_ctx->io_err, 0, status);

	if (atomic_dec_and_test(&io_ctx->io_ref))
		complete(&io_ctx->io_done);
}

static int do_read_io(struct p2p_io_context *io_ctx,
		      struct block_device *bdev, unsigned int nsid,
		      u64 sector, unsigned int sector_nr, u64 paddr)
{
	struct nvme_command cmd = { };
	struct gendisk *disk = bdev->bd_disk;
	struct request_queue *queue = disk->queue;
	struct request *req;

	pr_debug("read bdev %u:%u nsid %u sector 0x%llx nr_sectors 0x%x pa 0x%llx\n",
		 MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev), nsid, sector, sector_nr, paddr);

	cmd.rw.opcode = nvme_cmd_read;
	cmd.rw.nsid = cpu_to_le32(nsid);
	cmd.rw.slba = cpu_to_le64(sector);
	cmd.rw.length = cpu_to_le16(sector_nr - 1);
	cmd.rw.control = 0;
	cmd.rw.dsmgmt = 0;

	cmd.rw.dptr.sgl.addr = cpu_to_le64(paddr);
	cmd.rw.dptr.sgl.length = cpu_to_le32(sector_nr << SECTOR_SHIFT);
	cmd.rw.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;

	req = p2p_alloc_nvme_read_request(queue, &cmd);
	if (IS_ERR(req)) {
		pr_err("failed to allocate request: %ld\n",
		       PTR_ERR(req));
		return PTR_ERR(req);
	}

	req->end_io_data = io_ctx;
	req->end_io = p2p_end_read_io;
	atomic_inc(&io_ctx->io_ref);
	p2p_stats_io_issued((u64)sector_nr << SECTOR_SHIFT);

	p2p_execute_rq_nowait(req, disk);

	return 0;
}

static int do_read_ios(struct p2p_io_context *io_ctx, struct topo *topo,
		       struct fiemap_extent *extents, unsigned int nr)
{
	struct p2p_iov_iter iter;
	unsigned int i;
	int err = 0;

	p2p_iov_iter_init(&iter, io_ctx->pa_iov, io_ctx->pa_iov_nr,
			  io_ctx->data_size);

	for (i = 0; i < nr && p2p_iov_iter_count(&iter); i++) {
		u64 sector = extents[i].fe_physical >> SECTOR_SHIFT;
		/* The current use case limits each extent to less than 2 TiB. */
		u32 left = extents[i].fe_length >> SECTOR_SHIFT;
		unsigned int to_read;

		pr_debug("ext %u sec 0x%llx+0x%x cnt 0x%llx off 0x%llx cur 0x%llx+0x%llx nr %u\n",
			 i, sector, left, iter.count, iter.iov_offset,
			 iter.iov->addr, iter.iov->len, iter.nr_segs);
		while (left && p2p_iov_iter_count(&iter)) {
			struct topo_bdev *topo_bdev;
			struct block_device *to_bdev;
			u64 to_sector;
			u32 topo_limit = left;

			err = topo->ops->map_sector(topo, sector, topo_limit,
						     &topo_bdev, &to_sector,
						     &topo_limit);
			if (err)
				goto out;
			to_bdev = p2p_handle_to_bdev(topo_bdev->handle);

			to_read = min3(topo_limit,
				       p2p_queue_max_sectors(to_bdev),
				       p2p_iov_iter_single_seg_sectors(&iter));
			if (!to_read) {
				pr_err("zero read ext %u sectors 0x%x iov bytes 0x%llx\n",
				       i, left,
				       p2p_iov_iter_count(&iter));
				err = -EINVAL;
				goto out;
			}
			err = do_read_io(io_ctx, to_bdev, topo_bdev->nsid,
					 to_sector, to_read,
					 p2p_iov_iter_addr(&iter));
			if (err)
				goto out;
			sector += to_read;
			left -= to_read;

			p2p_iov_iter_advance(&iter, (u64)to_read << SECTOR_SHIFT);
		}
	}
	if (p2p_iov_iter_count(&iter)) {
		pr_err("source extents leave 0x%llx IOV bytes unread\n",
		       p2p_iov_iter_count(&iter));
		err = -EINVAL;
	}

out:
	return err;
}

static int wait_io_done(struct p2p_io_context *io_ctx)
{
	if (!atomic_dec_and_test(&io_ctx->io_ref))
		wait_for_completion_io(&io_ctx->io_done);
	return io_ctx->io_err;
}

#ifdef CALC_CRC32
static void dump_io_ctx_crc32(const struct p2p_io_context *io_ctx)
{
	u32 crc = ~0U;
	unsigned int i;

	/*
	 * wait_io_done() observed NVMe DMA completion before this helper.
	 * Order the following CPU reads from P2P memory after that completion.
	 */
	dma_rmb();

	for (i = 0; i < io_ctx->pa_iov_nr; i++) {
		const struct p2p_pa_iov *iov = &io_ctx->pa_iov[i];
		void __iomem *addr;

		addr = ioremap(iov->addr, iov->len);
		if (!addr) {
			pr_err("crc32 failed file=%pD length=0x%llx err=%d\n",
			       io_ctx->file, io_ctx->data_size, -ENOMEM);
			return;
		}

		crc = crc32_le(crc, (__force const unsigned char *)addr,
			       iov->len);
		iounmap(addr);
	}

	crc ^= ~0U;
	pr_info("crc32 file=%pD length=0x%llx crc32=0x%08x\n",
		io_ctx->file, io_ctx->data_size, crc);
}
#else
static void dump_io_ctx_crc32(const struct p2p_io_context *io_ctx)
{
}
#endif

static int validate_read_data(const struct p2p_iov *iov, unsigned int iov_nr,
			      const struct fiemap_extent *extents,
			      unsigned int ext_nr, u64 *data_size)
{
	u64 extent_size = 0;
	u64 iov_size = 0;
	unsigned int i;

	for (i = 0; i < iov_nr; i++) {
		u64 end;

		if (!iov[i].size ||
		    ((iov[i].addr | iov[i].size) & (SECTOR_SIZE - 1)) ||
		    check_add_overflow((u64)iov[i].addr, (u64)iov[i].size,
				       &end) ||
		    check_add_overflow(iov_size, (u64)iov[i].size,
				       &iov_size))
			return -EINVAL;
	}

	for (i = 0; i < ext_nr; i++) {
		u64 end;

		if (!extents[i].fe_length ||
		    ((extents[i].fe_physical | extents[i].fe_length) &
		     (SECTOR_SIZE - 1)) ||
		    check_add_overflow(extents[i].fe_physical,
				       extents[i].fe_length, &end) ||
		    check_add_overflow(extent_size, extents[i].fe_length,
				       &extent_size))
			return -EINVAL;
	}

	if (extent_size < iov_size)
		return -E2BIG;
	*data_size = iov_size;
	return 0;
}

static struct block_device *p2p_bdev_from_file(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb;

	if (S_ISBLK(inode->i_mode))
		return I_BDEV(file->f_mapping->host);

	if (!S_ISREG(inode->i_mode))
		return ERR_PTR(-EINVAL);

	sb = inode->i_sb;
	if (!sb || !sb->s_bdev)
		return ERR_PTR(-EOPNOTSUPP);

	return sb->s_bdev;
}

static int p2p_read_file(struct p2p_batch *batch, void __user *arg)
{
	struct p2p_read_param __user *user_param = arg;
	struct p2p_pa_iov *pa_iov = NULL;
	struct p2p_read_param param;
	struct fiemap_extent *extents;
	struct p2p_iov *iov;
	struct file *reg_file = NULL;
	struct block_device *bdev;
	struct p2p_io_context *io_ctx;
	struct p2p_pinned_pa *pinned_pa = NULL;
	struct topo *topo = NULL;
	u64 data_size;
	unsigned int pa_iov_nr;
	int err;

	if (copy_from_user(&param, user_param, sizeof(param)))
		return -EFAULT;
	if (param.host_pid <= 0 || !param.iov_nr ||
	    param.iov_nr > P2P_MAX_IOV || !param.ext_nr ||
	    param.ext_nr > P2P_MAX_EXTENTS)
		return -EINVAL;

	iov = kvmalloc_array(param.iov_nr, sizeof(*iov), GFP_KERNEL);
	if (!iov)
		return -ENOMEM;
	if (copy_from_user(iov,
			   (__force void __user *)(uintptr_t)param.iov,
			   array_size(param.iov_nr, sizeof(*iov)))) {
		err = -EFAULT;
		goto free_iov;
	}

	extents = kvmalloc_array(param.ext_nr, sizeof(*extents), GFP_KERNEL);
	if (!extents) {
		err = -ENOMEM;
		goto free_iov;
	}
	if (copy_from_user(extents, user_param->extents,
			   array_size(param.ext_nr, sizeof(*extents)))) {
		err = -EFAULT;
		goto free_extents;
	}

	err = validate_read_data(iov, param.iov_nr, extents, param.ext_nr,
				 &data_size);
	if (err)
		goto free_extents;

	reg_file = fget(param.file_fd);
	if (!reg_file) {
		err = -EBADF;
		goto free_extents;
	}
	bdev = p2p_bdev_from_file(reg_file);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		goto put_reg_file;
	}
	topo = topo_get(bdev);
	if (IS_ERR(topo)) {
		err = PTR_ERR(topo);
		goto put_reg_file;
	}
	if (!topo) {
		pr_err_ratelimited("topology for bdev %u:%u is not registered\n",
				   MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev));
		err = -ENODEV;
		goto put_reg_file;
	}

	err = get_pa_iov(param.host_pid, iov, param.iov_nr, &pa_iov, &pa_iov_nr, &pinned_pa);
	if (err)
		goto put_topo;

	io_ctx = new_io_ctx(reg_file, pa_iov, pa_iov_nr, pinned_pa, data_size);
	if (IS_ERR(io_ctx)) {
		err = PTR_ERR(io_ctx);
		goto free_pa_iov;
	}
	reg_file = NULL;
	pa_iov = NULL;
	pinned_pa = NULL;

	io_ctx->issue_err = do_read_ios(io_ctx, topo, extents, param.ext_nr);
	if (io_ctx->issue_err)
		p2p_stats_io_issue_failed();
	topo_put(topo);
	topo = NULL;

	spin_lock(&batch->io_lock);
	batch->io_cnt++;
	list_add_tail(&io_ctx->io_list, &batch->io_list);
	spin_unlock(&batch->io_lock);

free_pa_iov:
	p2p_put_pinned_pa(pinned_pa);
	kvfree(pa_iov);
put_topo:
	if (topo)
		topo_put(topo);
put_reg_file:
	if (reg_file)
		fput(reg_file);
free_extents:
	kvfree(extents);
free_iov:
	kvfree(iov);
	return err;
}

static int p2p_drain_io(struct p2p_batch *batch)
{
	struct p2p_io_context *io_ctx, *next_io_ctx;
	int first_err = 0;
	LIST_HEAD(tmp);

	if (!READ_ONCE(batch->io_cnt))
		return 0;

	spin_lock(&batch->io_lock);
	if (!batch->io_cnt) {
		spin_unlock(&batch->io_lock);
		return 0;
	}
	list_splice_init(&batch->io_list, &tmp);
	batch->io_cnt = 0;
	spin_unlock(&batch->io_lock);

	list_for_each_entry_safe(io_ctx, next_io_ctx, &tmp, io_list) {
		int err, io_err;

		err = io_ctx->issue_err;
		io_err = wait_io_done(io_ctx);

		if (io_err && !err) {
			pr_err("I/O error status %d errno %d\n", io_err,
			       blk_status_to_errno(io_err));
			err = blk_status_to_errno(io_err);
		}

		if (!err) {
			dump_io_ctx_crc32(io_ctx);
			dump_pa_content(io_ctx->pa_iov[0].addr,
					min_t(u64, io_ctx->data_size,
					      io_ctx->pa_iov[0].len));
		} else if (!first_err) {
			first_err = err;
		}

		list_del_init(&io_ctx->io_list);
		free_io_ctx(io_ctx);
	}

	return first_err;
}

static int p2p_add_topo(struct p2p_batch *batch, void __user *arg)
{
	struct topo_user_cfg header;
	struct topo_user_cfg *cfg;
	size_t size;
	int err;

	if (copy_from_user(&header, arg, sizeof(header)))
		return -EFAULT;
	if (!header.nr_devs)
		return -EINVAL;
	if (header.nr_devs > P2P_TOPO_MAX_BDEVS)
		return -E2BIG;
	size = sizeof(header) +
	       header.nr_devs * sizeof(header.bdevs[0]);

	cfg = memdup_user(arg, size);
	if (IS_ERR(cfg))
		return PTR_ERR(cfg);
	if (cfg->nr_devs != header.nr_devs) {
		kfree(cfg);
		return -EINVAL;
	}

	err = topo_add(cfg, &batch->shared_topos);
	kfree(cfg);
	return err;
}

static long p2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct p2p_batch *batch = file->private_data;
	int err = 0;

	switch (cmd) {
	case IOCTL_READ_FILE:
		err = p2p_read_file(batch, (void __user *)arg);
		break;
	case IOCTL_DRAIN_IO:
		err = p2p_drain_io(batch);
		break;
	case IOCTL_ADD_TOPO:
		err = p2p_add_topo(batch, (void __user *)arg);
		break;
	default:
		pr_info("invalid ioctl command 0x%x\n", cmd);
		err = -EINVAL;
		break;
	}
	return err;
}

static int __init p2p_drv_init(void)
{
	struct device *device;
	int err;

	err = topo_init();
	if (err) {
		pr_err("initialize topology release workqueue err %d\n", err);
		return err;
	}

	err = p2p_tp_hook_init();
	if (err) {
		pr_err("register tp hook err %d\n", err);
		goto exit_topo;
	}

	p2p_debugfs_init();

	err = alloc_chrdev_region(&dev, 0, 1, "p2p_device");
	if (err < 0) {
		pr_err("allocate major number err %d\n", err);
		goto exit_debugfs;
	}

	pr_info("major = %d minor = %d\n", MAJOR(dev), MINOR(dev));
	cdev_init(&p2p_cdev, &fops);

	err = cdev_add(&p2p_cdev, dev, 1);
	if (err < 0) {
		pr_err("add the device to the system err %d\n", err);
		goto free_dev;
	}

	dev_class = p2p_class_create("p2p_class");
	if (IS_ERR(dev_class)) {
		err = PTR_ERR(dev_class);
		pr_err("create class err %d\n", err);
		goto del_cdev;
	}

	device = device_create(dev_class, NULL, dev, NULL, "p2p_device");
	if (IS_ERR(device)) {
		err = PTR_ERR(device);
		pr_err("create device err %d\n", err);
		goto del_cls;
	}

	pr_info("driver inserted done\n");
	return 0;

del_cls:
	class_destroy(dev_class);
del_cdev:
	cdev_del(&p2p_cdev);
free_dev:
	unregister_chrdev_region(dev, 1);
exit_debugfs:
	p2p_debugfs_exit();
	p2p_tp_hook_exit();
exit_topo:
	topo_exit();
	return err;
}

static void __exit p2p_drv_exit(void)
{
	p2p_debugfs_exit();
	device_destroy(dev_class, dev);
	class_destroy(dev_class);
	cdev_del(&p2p_cdev);
	unregister_chrdev_region(dev, 1);
	p2p_tp_hook_exit();
	topo_exit();
	pr_info("driver removed done\n");
}

module_init(p2p_drv_init);
module_exit(p2p_drv_exit);
MODULE_DESCRIPTION("XDS NVMe peer-to-peer read driver");
MODULE_LICENSE("GPL");
