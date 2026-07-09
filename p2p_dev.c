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

u64 g_time = 0;
u64 g_count = 0;
u64 g_size = 0;

struct p2p_io_context {
	struct block_device *bdev;
	struct file *file;
	unsigned int nsid;
	unsigned int pa_num;
	unsigned int pa_idx;
	unsigned int pa_offset;
	u64 *pa_list;
	unsigned int pa_size;
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

dev_t dev = 0;
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
	if (!batch) {
		return -ENOMEM;
	}

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

	if (!list_empty(&batch->io_list) || batch->io_cnt > 0) {
		p2p_drain_read(batch);
	}

	kfree(batch);

	file->private_data = NULL;

	return 0;
}

static void init_process_id(const struct va_desc *desc,
			    struct devmm_svm_process_id *process_id)
{
	memset(process_id, 0, sizeof(*process_id));
	rcu_read_lock();
	process_id->host_pid = pid_nr(find_vpid(desc->hostpid));
	rcu_read_unlock();
}

static void init_process_id_batch(const struct va_desc_ba *desc,
				  struct devmm_svm_process_id *process_id)
{
	memset(process_id, 0, sizeof(*process_id));
	rcu_read_lock();
	process_id->host_pid = pid_nr(find_vpid(desc->hostpid));
	rcu_read_unlock();
}

#ifdef DUMP_CONTENT
static void dump_pa_content(unsigned long pa, unsigned int size)
{
	unsigned int data_len = min(size, 16U << 10);
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
static void dump_pa_content(unsigned long pa, unsigned int size)
{
}
#endif

static int get_pa_list(const struct va_desc *desc, u64 **_pa_list,
		       unsigned int *_pa_num, unsigned int *_pa_size)
{
	struct devmm_svm_process_id pid;
	u64 addr, aligned_addr;
	u64 size, aligned_size;
	u64 *pa_list;
	unsigned int pa_num;
	int page_size;
	int err;

	init_process_id(desc, &pid);

	addr = desc->addr;
	size = desc->size;
	page_size = devmm_get_mem_page_size(&pid, addr, size);
	if (page_size <= 0) {
		printk(KERN_ERR
		       "Failed to get page size for addr %llx, size %llu\n",
		       addr, size);
		if (!page_size) {
			page_size = -EINVAL;
		}
		return page_size;
	}

	aligned_addr = round_down(addr, page_size);
	aligned_size = round_up((addr - aligned_addr + size), page_size);
	pa_num = aligned_size / page_size;

	pa_list = kvmalloc(pa_num * sizeof(*pa_list), GFP_KERNEL);
	if (!pa_list) {
		return -ENOMEM;
	}

	err = devmm_get_mem_pa_list(&pid, aligned_addr, aligned_size, pa_list,
				    pa_num);
	if (err) {
		printk(KERN_ERR
		       "Failed to get PA list for addr %llx, size %llu, err %d\n",
		       aligned_addr, aligned_size, err);
		kvfree(pa_list);
		return err;
	}

	devmm_put_mem_pa_list(&pid, aligned_addr, aligned_size, pa_list,
			      pa_num);

	*_pa_list = pa_list;
	*_pa_num = pa_num;
	*_pa_size = page_size;

	return addr - aligned_addr;
}

static int get_pa_list_batch(const struct va_desc_ba *desc, u64 **_pa_list,
			     unsigned int *_pa_num, unsigned int *_pa_size,
			     int **_addr_off, int **_ret_size)
{
	struct devmm_svm_process_id pid;
	u64 addr, aligned_addr;
	u64 size, aligned_size;
	u64 tail;
	u64 *pa_list;
	int *addr_off;
	int *ret_size;
	unsigned int pa_num = 0;
	int page_size;
	int err;
	int i, j;

	init_process_id_batch(desc, &pid);

	addr = desc->addr[0];
	size = desc->size[0];
	page_size = devmm_get_mem_page_size(&pid, addr, size);
	if (page_size <= 0) {
		printk(KERN_ERR
		       "Failed to get page size for addr %llx, size %llu\n",
		       addr, size);
		if (!page_size) {
			page_size = -EINVAL;
		}
		return page_size;
	}

	for (i = 0; i < desc->count; i++) {
		aligned_addr = round_down(desc->addr[i], page_size);
		aligned_size =
			round_up((desc->addr[i] - aligned_addr + desc->size[i]),
				 page_size);
		pa_num += aligned_size / page_size;
	}

	pa_list = kvmalloc(pa_num * sizeof(*pa_list), GFP_KERNEL);
	if (!pa_list) {
		return -ENOMEM;
	}
	addr_off = kvmalloc(pa_num * sizeof(*addr_off), GFP_KERNEL);
	if (!addr_off) {
		kvfree(pa_list);
		return -ENOMEM;
	}
	ret_size = kvmalloc(pa_num * sizeof(*ret_size), GFP_KERNEL);
	if (!ret_size) {
		kvfree(pa_list);
		kvfree(addr_off);
		return -ENOMEM;
	}

	pa_num = 0;
	for (i = 0; i < desc->count; i++) {
		aligned_addr = round_down(desc->addr[i], page_size);
		aligned_size =
			round_up((desc->addr[i] - aligned_addr + desc->size[i]),
				 page_size);
		pa_num += aligned_size / page_size;

		err = devmm_get_mem_pa_list(
			&pid, aligned_addr, aligned_size,
			pa_list + (pa_num - aligned_size / page_size),
			aligned_size / page_size);
		if (err) {
			printk(KERN_ERR
			       "Failed to get PA list for batch %d, err %d\n",
			       i, err);
			kvfree(pa_list);
			kvfree(addr_off);
			kvfree(ret_size);
			return err;
		}
		devmm_put_mem_pa_list(
			&pid, aligned_addr, aligned_size,
			pa_list + (pa_num - aligned_size / page_size),
			aligned_size / page_size);
		j = pa_num - (aligned_size / page_size);
		addr_off[j] = desc->addr[i] - aligned_addr;
		ret_size[j] = min_t(int, desc->size[i],
				    page_size - (desc->addr[i] - aligned_addr));
		j++;
		tail = (desc->addr[i] - aligned_addr + desc->size[i]) %
		       page_size;
		while (j < pa_num) {
			addr_off[j] = 0;
			ret_size[j] = page_size;
			if (j == pa_num - 1) {
				ret_size[j] = tail ? tail : page_size;
			}
			j++;
		}
	}

	*_pa_list = pa_list;
	*_pa_num = pa_num;
	*_pa_size = page_size;
	*_addr_off = addr_off;
	*_ret_size = ret_size;

	return 0;
}

static int dump_pa(void __user *arg)
{
	struct va_desc desc;
	u64 *pa_list;
	unsigned int pa_num, pa_size;
	int err;

	if (copy_from_user(&desc, arg, sizeof(desc))) {
		return -EFAULT;
	}

	err = get_pa_list(&desc, &pa_list, &pa_num, &pa_size);
	if (err) {
		return err;
	}

	kfree(pa_list);
	return 0;
}

static void free_io_ctx(struct p2p_io_context *io_ctx)
{
	kvfree(io_ctx->cmd_list);
	kvfree(io_ctx->pa_list);
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
					 struct file *file,
					 const struct read_desc *desc,
					 u64 *pa_list, unsigned int pa_num,
					 unsigned int pa_size,
					 u64 data_size)
{
	struct p2p_io_context *io_ctx;
	unsigned int max_sectors;
	unsigned int max_bytes;
	unsigned int nr;

	max_sectors = p2p_queue_max_sectors(bdev);
	max_bytes = max_sectors << SECTOR_SHIFT;
	nr = round_up(data_size, max_bytes) / max_bytes * 10;

	io_ctx = kzalloc(sizeof(*io_ctx), GFP_KERNEL);
	if (!io_ctx)
		return ERR_PTR(-ENOMEM);

	io_ctx->cmd_list =
		kvmalloc_array(nr, sizeof(*io_ctx->cmd_list), GFP_KERNEL);
	if (!io_ctx->cmd_list) {
		kfree(io_ctx);
		return ERR_PTR(-ENOMEM);
	}

	io_ctx->bdev = bdev;
	io_ctx->file = file;
	io_ctx->nsid = desc->nsid;
	io_ctx->pa_num = pa_num;
	io_ctx->pa_list = pa_list;
	io_ctx->pa_size = pa_size;
	io_ctx->data_size = data_size;
	io_ctx->max_sectors = max_sectors;
	io_ctx->count = nr;

	atomic_set(&io_ctx->io_ref, 1);
	INIT_LIST_HEAD(&io_ctx->io_list);
	init_completion(&io_ctx->io_done);

	return io_ctx;
}

static struct p2p_io_context *
new_io_ctx_batch(struct block_device *bdev, struct file *file,
		 const struct read_desc_ba *desc,
		 u64 *pa_list, unsigned int pa_num, unsigned int pa_size,
		 u64 data_size)
{
	struct p2p_io_context *io_ctx;
	unsigned int max_sectors;
	unsigned int max_bytes;
	unsigned int count;

	max_sectors = p2p_queue_max_sectors(bdev);
	max_bytes = max_sectors << SECTOR_SHIFT;
	count = round_up(data_size, max_sectors) / max_sectors * 10;

	io_ctx = kzalloc(sizeof(*io_ctx), GFP_KERNEL);
	if (!io_ctx)
		return ERR_PTR(-ENOMEM);

	io_ctx->bdev = bdev;
	io_ctx->file = file;
	io_ctx->nsid = desc->nsid;
	io_ctx->pa_num = pa_num;
	io_ctx->pa_list = pa_list;
	io_ctx->pa_size = pa_size;
	io_ctx->max_sectors = max_sectors;
	io_ctx->cmd_list =
		kvmalloc_array(count, sizeof(struct nvme_command), GFP_KERNEL);
	if (!io_ctx->cmd_list) {
		kfree(io_ctx);
		return ERR_PTR(-ENOMEM);
	}
	io_ctx->start_time = ktime_get_ns();
	io_ctx->data_size = data_size;
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

#define THRESH_NS (1000000UL)

static int do_read_io(struct p2p_io_context *io_ctx, unsigned long long sector,
		      unsigned int sector_nr, unsigned long long paddr)
{
	struct gendisk *disk = io_ctx->bdev->bd_disk;
	struct request_queue *queue = disk->queue;
	struct request *req;
	struct nvme_command *cmd = &io_ctx->cmd_list[io_ctx->cmd_id];
	if (io_ctx->cmd_id >= io_ctx->count) {
		pr_err("cmd_id %u >= count %u\n", io_ctx->cmd_id,
		       io_ctx->count);
		return -EINVAL;
	}

	pr_debug("read from blk 0x%x0x%llx to pa 0x%llx\n", sector_nr, sector,
		 paddr);

	cmd->rw.opcode = nvme_cmd_read;
	cmd->rw.nsid = cpu_to_le32(io_ctx->nsid);
	cmd->rw.slba = cpu_to_le64(sector);
	cmd->rw.length = cpu_to_le16(sector_nr - 1);
	cmd->rw.control = 0;
	cmd->rw.dsmgmt = 0;

	cmd->rw.dptr.sgl.addr = cpu_to_le64(paddr);
	cmd->rw.dptr.sgl.length = cpu_to_le32(sector_nr << SECTOR_SHIFT);
	cmd->rw.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;
	io_ctx->cmd_id++;

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

	blk_execute_rq_nowait(req, true);

	return 0;
}

static unsigned int cur_pa_remain_sector(struct p2p_io_context *io_ctx)
{
	if (io_ctx->pa_idx >= io_ctx->pa_num) {
		pr_warn("bad pa_idx %d >= pa_num %d\n", io_ctx->pa_idx,
			io_ctx->pa_num);
		return 0;
	}
	return (io_ctx->pa_size - io_ctx->pa_offset) >> SECTOR_SHIFT;
}

static unsigned long long cur_pa(struct p2p_io_context *io_ctx)
{
	return io_ctx->pa_list[io_ctx->pa_idx] + io_ctx->pa_offset;
}

static void cur_pa_advance_sector(struct p2p_io_context *io_ctx,
				  unsigned int sector)
{
	io_ctx->pa_offset += (sector << SECTOR_SHIFT);
	if (io_ctx->pa_offset >= io_ctx->pa_size) {
		if (io_ctx->pa_offset > io_ctx->pa_size) {
			pr_warn("bad pa_offset %u > pa_size %u\n",
				io_ctx->pa_offset, io_ctx->pa_size);
		}
		io_ctx->pa_offset = 0;
		io_ctx->pa_idx++;
	}
}

static unsigned int calc_read_size(struct p2p_io_context *io_ctx,
				   unsigned int left)
{
	unsigned int limit = io_ctx->max_sectors;
	unsigned int to_read = left;
	unsigned int pa_left;

	if (to_read > limit)
		to_read = limit;

	pa_left = cur_pa_remain_sector(io_ctx);
	if (to_read > pa_left)
		to_read = pa_left;
	return to_read;
}

static int do_read_ios(struct p2p_io_context *io_ctx,
		       struct fiemap_extent *extents, unsigned int nr)
{
	unsigned int i;
	int err = 0;

	for (i = 0; i < nr; i++) {
		unsigned long long sector = extents[i].fe_physical >>
					    SECTOR_SHIFT;
		unsigned int left = extents[i].fe_length >> SECTOR_SHIFT;
		unsigned int to_read;
		int err;

		while (left > 0) {
			to_read = calc_read_size(io_ctx, left);
			if (!to_read) {
				err = -EINVAL;
				break;
			}
			err = do_read_io(io_ctx, sector, to_read,
					 cur_pa(io_ctx));
			if (err) {
				goto out;
			}
			sector += to_read;
			left -= to_read;
			cur_pa_advance_sector(io_ctx, to_read);
		}
	}

out:
	return err;
}

static int do_read_ios_batch(struct p2p_io_context *io_ctx,
			     struct fiemap_extent *extents, unsigned int nr,
			     int *addr_off, int *align_size)
{
	unsigned int i;
	int err = 0;
	unsigned long long count = 0;
	unsigned long long size = align_size[0] >> SECTOR_SHIFT;
	int idx = 0;
	io_ctx->pa_offset = addr_off[0];

	for (i = 0; i < nr; i++) {
		unsigned long long sector = extents[i].fe_physical >>
					    SECTOR_SHIFT;
		unsigned int left = extents[i].fe_length >> SECTOR_SHIFT;
		unsigned int to_read;
		int err;

		while (left > 0) {
			to_read = calc_read_size(io_ctx, left);
			if (!to_read) {
				err = -EINVAL;
				break;
			}

			if (count + to_read > size) {
				to_read = size - count;
				err = do_read_io(io_ctx, sector, to_read,
						 cur_pa(io_ctx));
				if (err) {
					goto out;
				}
				io_ctx->pa_idx++;
				idx++;
				if (idx >= io_ctx->pa_num) {
					pr_err("batch VA boundary overrun: idx %u >= pa_num %u\n",
					       idx, io_ctx->pa_num);
					err = -EINVAL;
					goto out;
				}
				io_ctx->pa_offset = addr_off[idx];
				size = align_size[idx] >> SECTOR_SHIFT;
				count = 0;
				sector += to_read;
				left -= to_read;
				continue;
			}

			err = do_read_io(io_ctx, sector, to_read,
					 cur_pa(io_ctx));
			if (err) {
				goto out;
			}
			sector += to_read;
			left -= to_read;
			count += to_read;
			if (count >= size) {
				io_ctx->pa_idx++;
				idx++;
				if (idx >= io_ctx->pa_num) {
					pr_err("batch VA boundary overrun: idx %u >= pa_num %u\n",
					       idx, io_ctx->pa_num);
					err = -EINVAL;
					goto out;
				}
				io_ctx->pa_offset = addr_off[idx];
				size = align_size[idx] >> SECTOR_SHIFT;
				count = 0;
			} else
				cur_pa_advance_sector(io_ctx, to_read);
		}
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

static unsigned long long calc_data_size(struct fiemap_extent *extents,
					 unsigned int nr)
{
	unsigned int i;
	unsigned long long size = 0;

	for (i = 0; i < nr; i++) {
		size += extents[i].fe_length;
	}
	return size;
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
	struct read_desc __user *user_desc = arg;
	struct fiemap_extent *extents;
	struct read_desc desc;
	struct file *reg_file;
	struct block_device *bdev;
	struct p2p_io_context *io_ctx;
	u64 *pa_list;
	u64 data_size;
	unsigned int pa_num;
	unsigned int pa_size;
	unsigned int ext_num;
	int err;
	int addr_off;

	if (copy_from_user(&desc, user_desc, sizeof(desc))) {
		return -EFAULT;
	}

	err = get_pa_list(&desc.desc, &pa_list, &pa_num, &pa_size);
	if (err) {
		return err;
	}
	addr_off = err;

	reg_file = fget(desc.file_fd);
	if (!reg_file) {
		err = -EBADF;
		goto free_pa_out;
	}

	bdev = p2p_bdev_from_file(reg_file);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		goto put_reg_file_out;
	}

	err = p2p_validate_nvme_bdev(bdev);
	if (err)
		goto put_reg_file_out;

	ext_num = desc.ext_num;
	extents = kvmalloc(ext_num * sizeof(*extents), GFP_KERNEL);
	if (!extents) {
		err = -ENOMEM;
		goto put_reg_file_out;
	}
	if (copy_from_user(extents, user_desc->extents,
			   ext_num * sizeof(*extents))) {
		err = -EFAULT;
		goto free_ext_out;
	}

	data_size = calc_data_size(extents, ext_num);
	if (data_size > (unsigned long long)pa_size * pa_num) {
		pr_err("data_size %llu > pa_size %u * pa_num %u\n", data_size,
		       pa_size, pa_num);
		err = -E2BIG;
		goto free_ext_out;
	}

	io_ctx = new_io_ctx(bdev, reg_file, &desc, pa_list, pa_num, pa_size,
			    data_size);
	if (IS_ERR(io_ctx)) {
		err = PTR_ERR(io_ctx);
		goto free_ext_out;
	}
	reg_file = NULL;
	pa_list = NULL;
	io_ctx->pa_offset = addr_off;

	io_ctx->issue_err = do_read_ios(io_ctx, extents, ext_num);

	spin_lock(&batch_lock);
	batch->io_cnt++;
	list_add_tail(&io_ctx->io_list, &batch->io_list);
	spin_unlock(&batch_lock);

free_ext_out:
	kvfree(extents);
put_reg_file_out:
	if (reg_file)
		fput(reg_file);
free_pa_out:
	kvfree(pa_list);
	return err;
}

static int p2p_drain_read(struct p2p_batch *batch)
{
	struct p2p_io_context *io_ctx, *next_io_ctx;
	unsigned int total_cnt;
	unsigned int got_cnt;
	unsigned int err_cnt;
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
			err = io_err;
		}

		if (!err)
			dump_pa_content(io_ctx->pa_list[0],
					min_t(unsigned int, io_ctx->data_size,
					      io_ctx->pa_size));
		else
			err_cnt++;

		if (io_ctx->cmd_id != 0) {
			g_time += (io_ctx->end_time - io_ctx->start_time);
			g_size += io_ctx->data_size;
			g_count++;
		} else {
			pr_info("cmd_id %u\n", io_ctx->cmd_id);
		}

		list_del_init(&io_ctx->io_list);
		free_io_ctx(io_ctx);
	}

	if (g_count >= 1000) {
		pr_info("end drain %d read got cnt %d/%d/%llu io %llu/%llu bandwidth %llu\n",
			batch->batch_id, got_cnt, err_cnt, g_count, g_size,
			g_time, g_size / g_time);
		g_time = 0;
		g_size = 0;
		g_count = 0;
	}
	return 0;
}

static int p2p_read_file_batch(struct p2p_batch *batch, void __user *arg)
{
	struct read_desc_ba __user *user_desc = arg;
	struct fiemap_extent *extents;
	unsigned long *kaddr, *kaddr1;
	struct read_desc_ba desc;
	struct file *reg_file;
	struct block_device *bdev;
	struct p2p_io_context *io_ctx;
	u64 *pa_list;
	u64 data_size;
	unsigned int pa_num;
	unsigned int pa_size;
	unsigned int ext_num;
	int err;
	int *addr_off;
	int *align_size;

	if (copy_from_user(&desc, user_desc, sizeof(desc))) {
		return -EFAULT;
	}
	kaddr = kvmalloc(desc.desc.count * sizeof(unsigned long), GFP_KERNEL);
	if (!kaddr)
		return -ENOMEM;
	if (copy_from_user(kaddr,
			   (__force void __user *)(uintptr_t)desc.desc.addr,
			   desc.desc.count * sizeof(unsigned long))) {
		err = -EFAULT;
		goto free_kaddr_out;
	}
	desc.desc.addr = kaddr;

	kaddr1 = kvmalloc(desc.desc.count * sizeof(unsigned long), GFP_KERNEL);
	if (!kaddr1) {
		err = -ENOMEM;
		goto free_kaddr_out;
	}
	if (copy_from_user(kaddr1,
			   (__force void __user *)(uintptr_t)desc.desc.size,
			   desc.desc.count * sizeof(unsigned long))) {
		err = -EFAULT;
		goto free_kaddr1_out;
	}
	desc.desc.size = kaddr1;

	err = get_pa_list_batch(&desc.desc, &pa_list, &pa_num, &pa_size,
				&addr_off, &align_size);
	if (err) {
		err = -EFAULT;
		goto free_kaddr1_out;
	}

	reg_file = fget(desc.file_fd);
	if (!reg_file) {
		err = -EBADF;
		goto free_pa_out;
	}

	bdev = p2p_bdev_from_file(reg_file);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		goto put_reg_file_out;
	}

	err = p2p_validate_nvme_bdev(bdev);
	if (err)
		goto put_reg_file_out;

	ext_num = desc.ext_num;
	extents = kvmalloc(ext_num * sizeof(*extents), GFP_KERNEL);
	if (!extents) {
		err = -ENOMEM;
		goto put_reg_file_out;
	}
	if (copy_from_user(extents, user_desc->extents,
			   ext_num * sizeof(*extents))) {
		err = -EFAULT;
		goto free_ext_out;
	}

	data_size = calc_data_size(extents, ext_num);
	if (data_size > (unsigned long long)pa_size * pa_num) {
		pr_err("data_size %llu > pa_size %u * pa_num %u\n", data_size,
		       pa_size, pa_num);
		err = -E2BIG;
		goto free_ext_out;
	}

	io_ctx = new_io_ctx_batch(bdev, reg_file, &desc, pa_list, pa_num,
				  pa_size, data_size);
	if (IS_ERR(io_ctx)) {
		err = PTR_ERR(io_ctx);
		goto free_ext_out;
	}
	reg_file = NULL;
	pa_list = NULL;

	io_ctx->issue_err = do_read_ios_batch(io_ctx, extents, ext_num,
					      addr_off, align_size);

	spin_lock(&batch_lock);
	batch->io_cnt++;
	list_add_tail(&io_ctx->io_list, &batch->io_list);
	spin_unlock(&batch_lock);

free_ext_out:
	kvfree(extents);
put_reg_file_out:
	if (reg_file)
		fput(reg_file);
free_pa_out:
	kvfree(pa_list);
	kvfree(addr_off);
	kvfree(align_size);
free_kaddr1_out:
	kvfree(kaddr1);
free_kaddr_out:
	kvfree(kaddr);
	return err;
}

static long p2p_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct p2p_batch *batch = file->private_data;
	int err = 0;

	switch (cmd) {
	case IOCTL_DUMP_PA:
		err = dump_pa((void __user *)arg);
		break;
	case IOCTL_READ_FILE:
		err = p2p_read_file(batch, (void __user *)arg);
		break;
	case IOCTL_READ_FILE_BATCH:
		err = p2p_read_file_batch(batch, (void __user *)arg);
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
