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
#include <linux/ktime.h>
#include <linux/idr.h>
#include <linux/notifier.h>
#include <linux/namei.h>
#include <linux/tracepoint.h>
#include <linux/limits.h>
#include <linux/overflow.h>

#include <linux/nvme.h>
#include <linux/blk_types.h>
#include <linux/file.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>

#include <trace/events/block.h>

#include "p2p_dev_uapi.h"
#include "p2p_mem_query.h"

#define P2P_NVME_MAX_RW_SECTORS ((1U << 20) >> SECTOR_SHIFT)
#define P2P_SYSFS_BDEV_PATH_MAX 48
#define P2P_MAX_IOV 65536U
#define P2P_MAX_EXTENTS 1048576U
#define P2P_MIN_PAGE_SIZE (64U << 10)

u64 g_time = 0;
u64 g_count = 0;
u64 g_size = 0;

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
	u32 page_size;
	u32 pa_num;
};

struct p2p_io_context {
	struct block_device *bdev;
	struct file *file;
	unsigned int nsid;
	struct p2p_pa_iov *pa_iov;
	unsigned int pa_iov_nr;
	u64 data_size;
	unsigned int max_sectors;
	struct completion io_done;
	atomic_t io_ref;
	int io_err;
	int issue_err;
	struct list_head io_list;
	struct nvme_command *cmd_list;
	unsigned int cmd_id;
	unsigned int count;
	u64 start_time;
	u64 end_time;
};

struct p2p_batch {
	unsigned int batch_id;
	struct list_head io_list;
	unsigned int io_cnt;
	spinlock_t io_lock;
};

#define RQF_NVME_PT ((__force req_flags_t)(1 << 31))

extern void nvme_init_request(struct request *req, struct nvme_command *cmd);

static void p2p_tp_hook_exit(void);

static dev_t dev;
static struct class *dev_class;
static struct cdev p2p_cdev;

static struct tracepoint *p2p_tp;
static DEFINE_MUTEX(p2p_tp_lock);
static struct module *p2p_tp_mod;

static DEFINE_SPINLOCK(batch_lock);
static DEFINE_IDR(batch_tree);

static int p2p_open(struct inode *inode, struct file *file);
static int p2p_release(struct inode *inode, struct file *file);
static long p2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int p2p_drain_read(struct p2p_batch *batch);

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = p2p_open,
	.release = p2p_release,
	.unlocked_ioctl = p2p_ioctl,
};

static int p2p_open(struct inode *inode, struct file *file)
{
	struct p2p_batch *batch;
	int err;

	batch = kzalloc(sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return -ENOMEM;

	spin_lock_init(&batch->io_lock);
	INIT_LIST_HEAD(&batch->io_list);
	batch->io_cnt = 0;

	idr_preload(GFP_KERNEL);
	spin_lock(&batch_lock);
	err = idr_alloc(&batch_tree, batch, 1, 0, GFP_ATOMIC);
	spin_unlock(&batch_lock);
	idr_preload_end();

	if (err < 0) {
		kfree(batch);
		return err;
	}

	batch->batch_id = err;
	file->private_data = batch;

	return 0;
}

static int p2p_release(struct inode *inode, struct file *file)
{
	struct p2p_batch *batch = file->private_data;

	spin_lock(&batch_lock);
	idr_remove(&batch_tree, batch->batch_id);
	spin_unlock(&batch_lock);

	if (!list_empty(&batch->io_list) || batch->io_cnt > 0)
		p2p_drain_read(batch);

	kfree(batch);

	file->private_data = NULL;

	return 0;
}

static void init_process_id(int host_pid,
			    struct devmm_svm_process_id *process_id)
{
	memset(process_id, 0, sizeof(*process_id));
	rcu_read_lock();
	process_id->host_pid = pid_nr(find_vpid(host_pid));
	rcu_read_unlock();
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

	print_hex_dump(KERN_INFO, "PA %llx content: ", pa, DUMP_PREFIX_ADDRESS,
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

static u64 p2p_iov_iter_single_seg_count(const struct p2p_iov_iter *iter)
{
	if (!iter->count)
		return 0;
	if (WARN_ON_ONCE(iter->iov_offset >= iter->iov->len))
		return 0;
	return iter->iov->len - iter->iov_offset;
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
		      unsigned int *pa_iov_nr)
{
	struct devmm_svm_process_id pid;
	struct p2p_pa_iov *new_pa_iov;
	struct p2p_iov_map *maps;
	u64 *pa_list;
	unsigned int max_pa_num = 0;
	unsigned int new_pa_iov_nr = 0;
	unsigned int new_pa_iov_idx = 0;
	unsigned int i;
	int err;

	init_process_id(host_pid, &pid);
	maps = kvmalloc_array(iov_nr, sizeof(*maps), GFP_KERNEL);
	if (!maps)
		return -ENOMEM;

	for (i = 0; i < iov_nr; i++) {
		u64 range_size;
		u64 pa_num;
		u64 offset;
		int page_size;

		page_size = devmm_get_mem_page_size(&pid, iov[i].addr,
						    iov[i].size);
		if (page_size < P2P_MIN_PAGE_SIZE ||
		    !is_power_of_2(page_size)) {
			pr_err("p2p: invalid page size %d for addr 0x%lx, iov %u\n",
			       page_size, iov[i].addr, i);
			err = -EINVAL;
			goto free_maps;
		}

		offset = iov[i].addr & (page_size - 1);
		range_size = offset + iov[i].size;
		pa_num = DIV_ROUND_UP_ULL(range_size, page_size);
		if (!pa_num || pa_num > UINT_MAX || pa_num > UINT_MAX - new_pa_iov_nr) {
			err = -E2BIG;
			goto free_maps;
		}

		maps[i].aligned_addr = iov[i].addr - offset;
		maps[i].page_size = page_size;
		maps[i].pa_num = pa_num;
		maps[i].aligned_size = pa_num * page_size;
		new_pa_iov_nr += maps[i].pa_num;
		max_pa_num = max(max_pa_num, maps[i].pa_num);
	}

	new_pa_iov = kvmalloc_array(new_pa_iov_nr, sizeof(*new_pa_iov),
				    GFP_KERNEL);
	if (!new_pa_iov) {
		err = -ENOMEM;
		goto free_maps;
	}
	pa_list = kvmalloc_array(max_pa_num, sizeof(*pa_list), GFP_KERNEL);
	if (!pa_list) {
		err = -ENOMEM;
		goto free_pa_iov;
	}

	for (i = 0; i < iov_nr; i++) {
		u64 remaining = iov[i].size;
		u64 offset = iov[i].addr - maps[i].aligned_addr;
		unsigned int j;

		err = devmm_get_mem_pa_list(&pid, maps[i].aligned_addr,
					    maps[i].aligned_size, pa_list,
					    maps[i].pa_num);
		if (err) {
			pr_err("p2p: failed to get PA list for addr 0x%llx, size 0x%llx, iov %u, err %d\n",
			       maps[i].aligned_addr, maps[i].aligned_size, i, err);
			goto free_pa_list;
		}

		for (j = 0; j < maps[i].pa_num; j++) {
			u64 len = min_t(u64, remaining, maps[i].page_size - offset);

			new_pa_iov[new_pa_iov_idx].addr = pa_list[j] + offset;
			new_pa_iov[new_pa_iov_idx].len = len;
			if ((new_pa_iov[new_pa_iov_idx].addr | len) & (SECTOR_SIZE - 1)) {
				pr_err("p2p: unaligned PA IOV addr 0x%llx, len 0x%llx, iov %u\n",
				       new_pa_iov[new_pa_iov_idx].addr, len, i);
				err = -EINVAL;
				break;
			}
			new_pa_iov_idx++;
			remaining -= len;
			offset = 0;
		}

		devmm_put_mem_pa_list(&pid, maps[i].aligned_addr,
					  maps[i].aligned_size, pa_list,
					  maps[i].pa_num);
		if (err)
			goto free_pa_list;
		if (remaining) {
			pr_err("p2p: PA list leaves 0x%llx bytes for iov %u\n",
			       remaining, i);
			err = -EINVAL;
			goto free_pa_list;
		}
	}

	kvfree(pa_list);
	kvfree(maps);
	*pa_iov = new_pa_iov;
	*pa_iov_nr = new_pa_iov_nr;
	return 0;

free_pa_list:
	kvfree(pa_list);
free_pa_iov:
	kvfree(new_pa_iov);
free_maps:
	kvfree(maps);
	return err;
}

static void free_io_ctx(struct p2p_io_context *io_ctx)
{
	kvfree(io_ctx->cmd_list);
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

static struct p2p_io_context *new_io_ctx(struct block_device *bdev,
					 struct file *file, unsigned int nsid,
					 struct p2p_pa_iov *pa_iov,
					 unsigned int pa_iov_nr, u64 data_size)
{
	struct p2p_io_context *io_ctx;
	unsigned int max_sectors;
	unsigned int max_bytes;
	u64 count;

	max_sectors = p2p_queue_max_sectors(bdev);
	max_bytes = max_sectors << SECTOR_SHIFT;
	count = DIV_ROUND_UP_ULL(data_size, max_bytes) * 10;

	io_ctx = kzalloc(sizeof(*io_ctx), GFP_KERNEL);
	if (!io_ctx)
		return ERR_PTR(-ENOMEM);

	io_ctx->cmd_list = kvcalloc(count, sizeof(*io_ctx->cmd_list), GFP_KERNEL);
	if (!io_ctx->cmd_list) {
		kfree(io_ctx);
		return ERR_PTR(-ENOMEM);
	}

	io_ctx->bdev = bdev;
	io_ctx->file = file;
	io_ctx->nsid = nsid;
	io_ctx->pa_iov = pa_iov;
	io_ctx->pa_iov_nr = pa_iov_nr;
	io_ctx->data_size = data_size;
	io_ctx->max_sectors = max_sectors;
	io_ctx->start_time = ktime_get_ns();
	io_ctx->count = count;

	atomic_set(&io_ctx->io_ref, 1);
	INIT_LIST_HEAD(&io_ctx->io_list);
	init_completion(&io_ctx->io_done);

	return io_ctx;
}

static void p2p_hook_nvme_setup_cmd(void *ignore, struct request *rq,
				    struct nvme_command *cmd)
{
	if (!(rq->rq_flags & RQF_NVME_PT))
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
		pr_info("p2p: registered nvme_setup_cmd tp hook\n");
		return 0;
	}

	pr_warn("p2p: register tp hook err %d\n", err);
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
			pr_warn("p2p: failed to pin module\n");
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
		pr_err("p2p: register tp module notifier err %d\n", err);
		return err;
	}

	mutex_lock(&p2p_tp_lock);
	err = p2p_tp ? 0 : -ENOENT;
	mutex_unlock(&p2p_tp_lock);

	unregister_tracepoint_module_notifier(&p2p_tp_module_nb);

	if (err) {
		pr_err("p2p: tp not found\n");
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
		pr_warn("p2p: unregister tp hook err %d\n", err);

	tracepoint_synchronize_unregister();

	module_put(p2p_tp_mod);
}

static enum rq_end_io_ret end_read_io(struct request *req, blk_status_t status)
{
	struct p2p_io_context *io_ctx = req->end_io_data;
	if (status)
		cmpxchg(&io_ctx->io_err, 0, status);

	io_ctx->end_time = ktime_get_ns();
	if (!atomic_dec_and_test(&io_ctx->io_ref)) {
		return RQ_END_IO_FREE;
	}

	complete(&io_ctx->io_done);
	return RQ_END_IO_FREE;
}

static int do_read_io(struct p2p_io_context *io_ctx, u64 sector,
		      unsigned int sector_nr, u64 paddr)
{
	struct gendisk *disk = io_ctx->bdev->bd_disk;
	struct request_queue *queue = disk->queue;
	struct request *req;
	struct nvme_command *cmd;

	if (io_ctx->cmd_id >= io_ctx->count) {
		pr_err("cmd_id %u >= count %u\n", io_ctx->cmd_id,
		       io_ctx->count);
		return -EINVAL;
	}
	cmd = &io_ctx->cmd_list[io_ctx->cmd_id];

	pr_debug("read from blk 0x%x0x%llx to pa 0x%llx\n", sector_nr, sector, paddr);

	cmd->rw.opcode = nvme_cmd_read;
	cmd->rw.nsid = cpu_to_le32(io_ctx->nsid);
	cmd->rw.slba = cpu_to_le64(sector);
	cmd->rw.length = cpu_to_le16(sector_nr - 1);
	cmd->rw.control = 0;
	cmd->rw.dsmgmt = 0;

	cmd->rw.dptr.sgl.addr = cpu_to_le64(paddr);
	cmd->rw.dptr.sgl.length = cpu_to_le32(sector_nr << SECTOR_SHIFT);
	cmd->rw.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;

	req = blk_mq_alloc_request(queue, REQ_OP_DRV_IN, 0);
	if (IS_ERR(req)) {
		pr_err("Failed to alloc request\n");
		return -ENOMEM;
	}
	nvme_init_request(req, cmd);

	req->rq_flags |= RQF_NVME_PT;
	req->end_io_data = io_ctx;
	req->end_io = end_read_io;
	atomic_inc(&io_ctx->io_ref);
	io_ctx->cmd_id++;

	blk_execute_rq_nowait(req, true);

	return 0;
}

static unsigned int calc_read_size(struct p2p_io_context *io_ctx,
				   const struct p2p_iov_iter *iter,
				   u64 extent_left)
{
	u64 to_read;
	u64 iov_left;

	to_read = min_t(u64, extent_left, io_ctx->max_sectors);
	iov_left = p2p_iov_iter_single_seg_count(iter) >> SECTOR_SHIFT;
	return min(to_read, iov_left);
}

static int do_read_ios(struct p2p_io_context *io_ctx,
		       struct fiemap_extent *extents, unsigned int nr)
{
	struct p2p_iov_iter iter;
	unsigned int i;
	int err = 0;

	p2p_iov_iter_init(&iter, io_ctx->pa_iov, io_ctx->pa_iov_nr,
			  io_ctx->data_size);

	for (i = 0; i < nr && p2p_iov_iter_count(&iter); i++) {
		u64 sector = extents[i].fe_physical >> SECTOR_SHIFT;
		u64 left = extents[i].fe_length >> SECTOR_SHIFT;
		unsigned int to_read;

		while (left > 0 && p2p_iov_iter_count(&iter)) {
			to_read = calc_read_size(io_ctx, &iter, left);
			if (!to_read) {
				pr_err("p2p: zero read size at extent %u, extent sectors 0x%llx, IOV bytes 0x%llx\n",
				       i, left, p2p_iov_iter_count(&iter));
				err = -EINVAL;
				goto out;
			}
			err = do_read_io(io_ctx, sector, to_read,
					 p2p_iov_iter_addr(&iter));
			if (err)
				goto out;
			sector += to_read;
			left -= to_read;

			p2p_iov_iter_advance(&iter, (u64)to_read << SECTOR_SHIFT);
		}
	}
	if (p2p_iov_iter_count(&iter)) {
		pr_err("p2p: source extents leave 0x%llx IOV bytes unread\n",
		       p2p_iov_iter_count(&iter));
		err = -EINVAL;
	}

out:
	return err;
}

static int wait_io_done(struct p2p_io_context *io_ctx)
{
	if (!atomic_dec_and_test(&io_ctx->io_ref)) {
		wait_for_completion_io(&io_ctx->io_done);
	}
	return io_ctx->io_err;
}

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
		return I_BDEV(inode);

	if (!S_ISREG(inode->i_mode))
		return ERR_PTR(-EINVAL);

	sb = inode->i_sb;
	if (!sb || !sb->s_bdev)
		return ERR_PTR(-EOPNOTSUPP);

	return sb->s_bdev;
}

static bool p2p_sysfs_bdev_attr_exists(dev_t devt, const char *attr)
{
	char name[P2P_SYSFS_BDEV_PATH_MAX];
	struct path path;
	int len;
	int err;

	len = snprintf(name, sizeof(name), "/sys/dev/block/%u:%u/device/%s",
		       MAJOR(devt), MINOR(devt), attr);
	WARN_ON_ONCE(len >= (int)sizeof(name));

	err = kern_path(name, LOOKUP_FOLLOW, &path);
	if (err)
		return false;
	path_put(&path);
	return true;
}

static int p2p_validate_nvme_bdev(struct block_device *bdev)
{
	dev_t devt = bdev->bd_dev;

	if (p2p_sysfs_bdev_attr_exists(devt, "subsysnqn")) {
		if (p2p_sysfs_bdev_attr_exists(devt, "iopolicy")) {
			pr_err("p2p: NVMe multipath head %u:%u is unsupported\n",
			       MAJOR(devt), MINOR(devt));
			return -EOPNOTSUPP;
		}
		return 0;
	}

	pr_err("p2p: bdev %u:%u is not a supported NVMe namespace\n",
	       MAJOR(devt), MINOR(devt));
	return -ENODEV;
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
	u64 data_size;
	unsigned int pa_iov_nr;
	int err;

	if (copy_from_user(&param, user_param, sizeof(param)))
		return -EFAULT;
	if (!param.nsid || param.host_pid <= 0 || !param.iov_nr ||
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
	err = p2p_validate_nvme_bdev(bdev);
	if (err)
		goto put_reg_file;

	err = get_pa_iov(param.host_pid, iov, param.iov_nr, &pa_iov, &pa_iov_nr);
	if (err)
		goto put_reg_file;

	io_ctx = new_io_ctx(bdev, reg_file, param.nsid, pa_iov, pa_iov_nr, data_size);
	if (IS_ERR(io_ctx)) {
		err = PTR_ERR(io_ctx);
		goto free_pa_iov;
	}
	reg_file = NULL;
	pa_iov = NULL;

	io_ctx->issue_err = do_read_ios(io_ctx, extents, param.ext_nr);

	spin_lock(&batch_lock);
	batch->io_cnt++;
	list_add_tail(&io_ctx->io_list, &batch->io_list);
	spin_unlock(&batch_lock);

free_pa_iov:
	kvfree(pa_iov);
put_reg_file:
	if (reg_file)
		fput(reg_file);
free_extents:
	kvfree(extents);
free_iov:
	kvfree(iov);
	return err;
}

static int p2p_drain_read(struct p2p_batch *batch)
{
	struct p2p_io_context *io_ctx, *next_io_ctx;
	unsigned int total_cnt;
	unsigned int got_cnt;
	unsigned int err_cnt;
	int first_err = 0;
	LIST_HEAD(tmp);

	if (!READ_ONCE(batch->io_cnt))
		return 0;

	spin_lock(&batch_lock);
	list_splice_init(&batch->io_list, &tmp);
	total_cnt = batch->io_cnt;
	batch->io_cnt = 0;
	spin_unlock(&batch_lock);

	if (total_cnt == 0)
		return 0;

	err_cnt = 0;
	got_cnt = 0;
	list_for_each_entry_safe(io_ctx, next_io_ctx, &tmp, io_list) {
		int err, io_err;

		got_cnt++;
		err = io_ctx->issue_err;
		io_err = wait_io_done(io_ctx);

		if (io_err && !err) {
			pr_err("got io err %d %d %d\n", io_err,
			       blk_status_to_errno(io_err), io_ctx->nsid);
			err = blk_status_to_errno(io_err);
		}

		if (!err)
			dump_pa_content(io_ctx->pa_iov[0].addr,
					min_t(u64, io_ctx->data_size,
					      io_ctx->pa_iov[0].len));
		else {
			err_cnt++;
			if (!first_err)
				first_err = err;
		}

		if (!err && io_ctx->cmd_id != 0) {
			g_time += (io_ctx->end_time - io_ctx->start_time);
			g_size += io_ctx->data_size;
			g_count++;
		} else {
			pr_info("cmd_id %u\n", io_ctx->cmd_id);
		}

		list_del_init(&io_ctx->io_list);
		free_io_ctx(io_ctx);
	}

	if (g_count >= 1000 && g_time) {
		pr_info("end drain %d read got cnt %d/%d/%llu io %llu/%llu bandwidth %llu\n",
			batch->batch_id, got_cnt, err_cnt, g_count, g_size,
			g_time, g_size / g_time);
		g_time = 0;
		g_size = 0;
		g_count = 0;
	}
	return first_err;
}

static long p2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct p2p_batch *batch = file->private_data;
	int err = 0;

	switch (cmd) {
	case IOCTL_READ_FILE:
	case IOCTL_READ_FILE_BATCH:
		err = p2p_read_file(batch, (void __user *)arg);
		break;
	case IOCTL_DRAIN_READ:
		err = p2p_drain_read(batch);
		break;
	default:
		pr_info("p2p driver: invalid ioctl command 0x%x\n", cmd);
		err = -EINVAL;
		break;
	}
	return err;
}

static int __init p2p_drv_init(void)
{
	struct device *device;
	int err;

	err = p2p_tp_hook_init();
	if (err) {
		pr_err("p2p: register tp hook err %d\n", err);
		return err;
	}

	err = alloc_chrdev_region(&dev, 0, 1, "p2p_device");
	if (err < 0) {
		pr_err("p2p: cannot allocate major number err %d\n",
		       err);
		goto unregister_hook;
	}

	pr_info("p2p: Major = %d Minor = %d \n", MAJOR(dev), MINOR(dev));
	cdev_init(&p2p_cdev, &fops);

	err = cdev_add(&p2p_cdev, dev, 1);
	if (err < 0) {
		pr_err("p2p: Cannot add the device to the system. \n");
		goto free_dev;
	}

	dev_class = class_create("p2p_class");
	if (IS_ERR(dev_class)) {
		err = PTR_ERR(dev_class);
		pr_err("p2p: Cannot create the struct class\n");
		goto del_cdev;
	}

	device = device_create(dev_class, NULL, dev, NULL, "p2p_device");
	if (IS_ERR(device)) {
		err = PTR_ERR(device);
		pr_err("p2p: Cannot create the device\n");
		goto del_cls;
	}

	pr_info("p2p: Device Driver Inserted Done\n");
	return 0;

del_cls:
	class_destroy(dev_class);
del_cdev:
	cdev_del(&p2p_cdev);
free_dev:
	unregister_chrdev_region(dev, 1);
unregister_hook:
	p2p_tp_hook_exit();
	return err;
}

static void __exit p2p_drv_exit(void)
{
	p2p_tp_hook_exit();

	device_destroy(dev_class, dev);
	class_destroy(dev_class);
	cdev_del(&p2p_cdev);
	unregister_chrdev_region(dev, 1);
	pr_info("p2p: Device Driver Removed Done\n");
}

module_init(p2p_drv_init);
module_exit(p2p_drv_exit);
MODULE_LICENSE("GPL");
