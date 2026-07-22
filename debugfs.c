// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include "debugfs.h"

struct p2p_io_stats {
	atomic64_t io_issued;
	atomic64_t io_bytes;
	atomic64_t io_inflight;
	atomic64_t io_failed;
	atomic64_t io_issue_failed;
};

struct p2p_io_stats_snapshot {
	s64 io_issued;
	s64 io_bytes;
	s64 io_inflight;
	s64 io_failed;
	s64 io_issue_failed;
};

static struct p2p_io_stats p2p_io_stats = {
	.io_issued = ATOMIC64_INIT(0),
	.io_bytes = ATOMIC64_INIT(0),
	.io_inflight = ATOMIC64_INIT(0),
	.io_failed = ATOMIC64_INIT(0),
	.io_issue_failed = ATOMIC64_INIT(0),
};
static struct dentry *p2p_debugfs_root;

void p2p_stats_io_issued(u64 bytes)
{
	atomic64_inc(&p2p_io_stats.io_issued);
	atomic64_add(bytes, &p2p_io_stats.io_bytes);
	atomic64_inc(&p2p_io_stats.io_inflight);
}

void p2p_stats_io_issue_failed(void)
{
	atomic64_inc(&p2p_io_stats.io_issue_failed);
}

void p2p_stats_io_complete(blk_status_t status)
{
	if (status)
		atomic64_inc(&p2p_io_stats.io_failed);
	atomic64_dec(&p2p_io_stats.io_inflight);
}

static int p2p_summary_show(struct seq_file *seq, void *unused)
{
	struct p2p_io_stats_snapshot snapshot = {
		.io_issued = atomic64_read(&p2p_io_stats.io_issued),
		.io_bytes = atomic64_read(&p2p_io_stats.io_bytes),
		.io_inflight = atomic64_read(&p2p_io_stats.io_inflight),
		.io_failed = atomic64_read(&p2p_io_stats.io_failed),
		.io_issue_failed = atomic64_read(&p2p_io_stats.io_issue_failed),
	};

	seq_printf(seq, "io_issued: %lld\n", snapshot.io_issued);
	seq_printf(seq, "io_bytes: %lld\n", snapshot.io_bytes);
	seq_printf(seq, "io_inflight: %lld\n", snapshot.io_inflight);
	seq_printf(seq, "io_failed: %lld\n", snapshot.io_failed);
	seq_printf(seq, "io_issue_failed: %lld\n", snapshot.io_issue_failed);
	return 0;
}

static int p2p_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, p2p_summary_show, NULL);
}

static ssize_t p2p_summary_write(struct file *file,
				 const char __user *buf, size_t count,
				 loff_t *ppos)
{
	if (!count)
		return 0;

	atomic64_xchg(&p2p_io_stats.io_issued, 0);
	atomic64_xchg(&p2p_io_stats.io_bytes, 0);
	atomic64_xchg(&p2p_io_stats.io_failed, 0);
	atomic64_xchg(&p2p_io_stats.io_issue_failed, 0);
	return count;
}

static const struct file_operations p2p_summary_fops = {
	.owner = THIS_MODULE,
	.open = p2p_summary_open,
	.read = seq_read,
	.write = p2p_summary_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void p2p_debugfs_init(void)
{
	struct dentry *root;
	struct dentry *summary;

	root = debugfs_create_dir("p2p_device", NULL);
	if (IS_ERR(root)) {
		pr_warn("p2p: create debugfs directory err %ld\n",
			PTR_ERR(root));
		return;
	}

	summary = debugfs_create_file("summary", 0600, root, NULL,
				      &p2p_summary_fops);
	if (IS_ERR(summary)) {
		pr_warn("p2p: create debugfs summary err %ld\n",
			PTR_ERR(summary));
		debugfs_remove_recursive(root);
		return;
	}

	p2p_debugfs_root = root;
}

void p2p_debugfs_exit(void)
{
	debugfs_remove_recursive(p2p_debugfs_root);
	p2p_debugfs_root = NULL;
}
