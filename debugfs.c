// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <asm/local.h>

#include "debugfs.h"

/*
 * Per-CPU I/O counters, same pattern as block layer disk_stats / part_stat:
 * Each field is indexed by P2P_IO_STATS_READ or P2P_IO_STATS_WRITE. Update
 * this CPU under preempt_disable() (io_inflight via local_t); readers sum
 * across CPUs.
 */
struct p2p_io_stats {
	u64 io_issued[P2P_IO_STATS_NR];
	u64 io_bytes[P2P_IO_STATS_NR];
	local_t io_inflight[P2P_IO_STATS_NR];
	u64 io_failed[P2P_IO_STATS_NR];
	u64 io_issue_failed[P2P_IO_STATS_NR];
	u64 lat_ns[P2P_TOPO_KIND_NR][P2P_LAT_NR];
	u64 lat_events[P2P_TOPO_KIND_NR][P2P_LAT_NR];
	u64 cmd_bytes[P2P_TOPO_KIND_NR];
	u64 cmd_size_bucket[P2P_TOPO_KIND_NR][P2P_LAT_SIZE_BUCKETS];
};

struct p2p_io_stats_snapshot {
	u64 io_issued[P2P_IO_STATS_NR];
	u64 io_bytes[P2P_IO_STATS_NR];
	s64 io_inflight[P2P_IO_STATS_NR];
	u64 io_failed[P2P_IO_STATS_NR];
	u64 io_issue_failed[P2P_IO_STATS_NR];
	u64 lat_ns[P2P_TOPO_KIND_NR][P2P_LAT_NR];
	u64 lat_events[P2P_TOPO_KIND_NR][P2P_LAT_NR];
	u64 cmd_bytes[P2P_TOPO_KIND_NR];
	u64 cmd_size_bucket[P2P_TOPO_KIND_NR][P2P_LAT_SIZE_BUCKETS];
};

static DEFINE_PER_CPU(struct p2p_io_stats, p2p_io_stats);
static struct dentry *p2p_debugfs_root;
static const char * const p2p_io_op_names[P2P_IO_STATS_NR] = {
	[P2P_IO_STATS_READ] = "read",
	[P2P_IO_STATS_WRITE] = "write",
};

static const char * const p2p_topo_kind_names[P2P_TOPO_KIND_NR] = {
	[P2P_TOPO_KIND_NVME] = "nvme",
	[P2P_TOPO_KIND_LINEAR] = "linear",
	[P2P_TOPO_KIND_RAID0] = "raid0",
	[P2P_TOPO_KIND_LOOP] = "loop",
};

static const char * const p2p_lat_names[P2P_LAT_NR] = {
	[P2P_LAT_PREP] = "prep",
	[P2P_LAT_MAP] = "map",
	[P2P_LAT_ISSUE] = "issue",
	[P2P_LAT_DEV] = "dev",
};

static inline u32 p2p_io_stats_index(u32 op)
{
	return !!op;
}

void p2p_stats_io_issued(u32 op, u64 bytes)
{
	struct p2p_io_stats *stats;
	u32 index = p2p_io_stats_index(op);

	preempt_disable();
	stats = this_cpu_ptr(&p2p_io_stats);
	stats->io_issued[index]++;
	stats->io_bytes[index] += bytes;
	local_inc(&stats->io_inflight[index]);
	preempt_enable();
}

void p2p_stats_io_issue_failed(u32 op)
{
	struct p2p_io_stats *stats;
	u32 index = p2p_io_stats_index(op);

	preempt_disable();
	stats = this_cpu_ptr(&p2p_io_stats);
	stats->io_issue_failed[index]++;
	preempt_enable();
}

void p2p_stats_io_complete(u32 op, blk_status_t status)
{
	struct p2p_io_stats *stats;
	u32 index = p2p_io_stats_index(op);

	preempt_disable();
	stats = this_cpu_ptr(&p2p_io_stats);
	if (status)
		stats->io_failed[index]++;
	local_dec(&stats->io_inflight[index]);
	preempt_enable();
}

static u32 p2p_cmd_size_bucket(u64 bytes)
{
	if (bytes <= 32U << 10)
		return 0;
	if (bytes <= 64U << 10)
		return 1;
	if (bytes <= 128U << 10)
		return 2;
	return 3;
}

void p2p_stats_lat_add(u8 kind, u32 stage, u64 ns, u64 bytes)
{
	struct p2p_io_stats *stats;

	if (kind >= P2P_TOPO_KIND_NR || stage >= P2P_LAT_NR)
		return;

	preempt_disable();
	stats = this_cpu_ptr(&p2p_io_stats);
	stats->lat_ns[kind][stage] += ns;
	stats->lat_events[kind][stage]++;
	if (stage == P2P_LAT_ISSUE && bytes) {
		stats->cmd_bytes[kind] += bytes;
		stats->cmd_size_bucket[kind][p2p_cmd_size_bucket(bytes)]++;
	}
	preempt_enable();
}

static void p2p_io_stats_snapshot(struct p2p_io_stats_snapshot *out)
{
	unsigned int cpu;
	u32 index;

	memset(out, 0, sizeof(*out));
	for_each_possible_cpu(cpu) {
		struct p2p_io_stats *s = per_cpu_ptr(&p2p_io_stats, cpu);

		for (index = 0; index < P2P_IO_STATS_NR; index++) {
			out->io_issued[index] += s->io_issued[index];
			out->io_bytes[index] += s->io_bytes[index];
			out->io_inflight[index] += local_read(&s->io_inflight[index]);
			out->io_failed[index] += s->io_failed[index];
			out->io_issue_failed[index] += s->io_issue_failed[index];
		}
		for (index = 0; index < P2P_TOPO_KIND_NR; index++) {
			u32 stage;

			for (stage = 0; stage < P2P_LAT_NR; stage++) {
				out->lat_ns[index][stage] += s->lat_ns[index][stage];
				out->lat_events[index][stage] +=
					s->lat_events[index][stage];
			}
			out->cmd_bytes[index] += s->cmd_bytes[index];
			for (stage = 0; stage < P2P_LAT_SIZE_BUCKETS; stage++)
				out->cmd_size_bucket[index][stage] +=
					s->cmd_size_bucket[index][stage];
		}
	}
}

static void p2p_summary_show_op(struct seq_file *seq, u32 index,
				const struct p2p_io_stats_snapshot *snapshot)
{
	const char *name = p2p_io_op_names[index];

	seq_printf(seq, "%s_io_issued: %llu\n", name, snapshot->io_issued[index]);
	seq_printf(seq, "%s_io_bytes: %llu\n", name, snapshot->io_bytes[index]);
	seq_printf(seq, "%s_io_inflight: %lld\n", name, snapshot->io_inflight[index]);
	seq_printf(seq, "%s_io_failed: %llu\n", name, snapshot->io_failed[index]);
	seq_printf(seq, "%s_io_issue_failed: %llu\n", name, snapshot->io_issue_failed[index]);
}

static void p2p_summary_show_lat(struct seq_file *seq, u32 kind,
				 const struct p2p_io_stats_snapshot *snapshot)
{
	const char *name = p2p_topo_kind_names[kind];
	u64 cmds = snapshot->lat_events[kind][P2P_LAT_ISSUE];
	u32 stage;

	if (!snapshot->lat_events[kind][P2P_LAT_PREP] && !cmds &&
	    !snapshot->lat_events[kind][P2P_LAT_DEV])
		return;

	seq_printf(seq, "%s_cmd_bytes: %llu\n", name, snapshot->cmd_bytes[kind]);
	seq_printf(seq, "%s_cmd_avg_kib: %llu\n", name,
		   cmds ? snapshot->cmd_bytes[kind] / cmds / 1024 : 0);
	seq_printf(seq, "%s_cmds_le_32kib: %llu\n", name,
		   snapshot->cmd_size_bucket[kind][0]);
	seq_printf(seq, "%s_cmds_le_64kib: %llu\n", name,
		   snapshot->cmd_size_bucket[kind][1]);
	seq_printf(seq, "%s_cmds_le_128kib: %llu\n", name,
		   snapshot->cmd_size_bucket[kind][2]);
	seq_printf(seq, "%s_cmds_gt_128kib: %llu\n", name,
		   snapshot->cmd_size_bucket[kind][3]);
	for (stage = 0; stage < P2P_LAT_NR; stage++) {
		u64 events = snapshot->lat_events[kind][stage];
		u64 total = snapshot->lat_ns[kind][stage];

		seq_printf(seq, "%s_%s_events: %llu\n", name,
			   p2p_lat_names[stage], events);
		seq_printf(seq, "%s_%s_total_ns: %llu\n", name,
			   p2p_lat_names[stage], total);
		seq_printf(seq, "%s_%s_avg_us: %llu\n", name, p2p_lat_names[stage],
			   events ? total / events / 1000 : 0);
	}
}

static int p2p_summary_show(struct seq_file *seq, void *unused)
{
	struct p2p_io_stats_snapshot snapshot;
	u32 index;

	p2p_io_stats_snapshot(&snapshot);

	seq_puts(seq, "# prep: logical IO before map (copy, pin, alloc)\n");
	seq_puts(seq, "# map: topology translation\n");
	seq_puts(seq, "# issue: queue one NVMe command, including tag wait\n");
	seq_puts(seq, "# dev: from queue until NVMe completion\n");
	for (index = 0; index < P2P_IO_STATS_NR; index++)
		p2p_summary_show_op(seq, index, &snapshot);
	for (index = 0; index < P2P_TOPO_KIND_NR; index++)
		p2p_summary_show_lat(seq, index, &snapshot);
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
	unsigned int cpu;
	u32 index;

	if (!count)
		return 0;

	/* Reset cumulative counters only; leave io_inflight alone. */
	for_each_possible_cpu(cpu) {
		struct p2p_io_stats *s = per_cpu_ptr(&p2p_io_stats, cpu);

		for (index = 0; index < P2P_IO_STATS_NR; index++) {
			s->io_issued[index] = 0;
			s->io_bytes[index] = 0;
			s->io_failed[index] = 0;
			s->io_issue_failed[index] = 0;
		}
		for (index = 0; index < P2P_TOPO_KIND_NR; index++) {
			u32 stage;

			for (stage = 0; stage < P2P_LAT_NR; stage++) {
				s->lat_ns[index][stage] = 0;
				s->lat_events[index][stage] = 0;
			}
			s->cmd_bytes[index] = 0;
			for (stage = 0; stage < P2P_LAT_SIZE_BUCKETS; stage++)
				s->cmd_size_bucket[index][stage] = 0;
		}
	}
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

static int p2p_memory_open(struct inode *inode, struct file *file)
{
	return single_open(file, p2p_debugfs_memory_show, NULL);
}

static const struct file_operations p2p_memory_fops = {
	.owner = THIS_MODULE,
	.open = p2p_memory_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int p2p_topo_open(struct inode *inode, struct file *file)
{
	return single_open(file, p2p_debugfs_topo_show, NULL);
}

static const struct file_operations p2p_topo_fops = {
	.owner = THIS_MODULE,
	.open = p2p_topo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void p2p_debugfs_init(void)
{
	p2p_debugfs_root = debugfs_create_dir("p2p_device", NULL);
	debugfs_create_file("summary", 0600, p2p_debugfs_root, NULL, &p2p_summary_fops);
	debugfs_create_file("memory", 0400, p2p_debugfs_root, NULL, &p2p_memory_fops);
	debugfs_create_file("topo", 0400, p2p_debugfs_root, NULL, &p2p_topo_fops);
}

void p2p_debugfs_exit(void)
{
	debugfs_remove_recursive(p2p_debugfs_root);
	p2p_debugfs_root = NULL;
}
