/* SPDX-License-Identifier: GPL-2.0 */
#ifndef P2P_DEBUGFS_H_
#define P2P_DEBUGFS_H_

#include <linux/blk_types.h>
#include <linux/types.h>

#define P2P_IO_STATS_READ 0U
#define P2P_IO_STATS_WRITE 1U
#define P2P_IO_STATS_NR 2U

#define P2P_TOPO_KIND_NVME 0U
#define P2P_TOPO_KIND_LINEAR 1U
#define P2P_TOPO_KIND_RAID0 2U
#define P2P_TOPO_KIND_LOOP 3U
#define P2P_TOPO_KIND_NR 4U

/* One logical IO before map_sector: copy, check, pin, allocate context. */
#define P2P_LAT_PREP 0U
/* Topology translation for one submitted piece. */
#define P2P_LAT_MAP 1U
/* Allocate and queue one NVMe command, including tag wait. */
#define P2P_LAT_ISSUE 2U
/* From queue until the NVMe completion callback. */
#define P2P_LAT_DEV 3U
#define P2P_LAT_NR 4U
#define P2P_LAT_SIZE_BUCKETS 4U

struct seq_file;

void p2p_debugfs_init(void);
void p2p_debugfs_exit(void);
int p2p_debugfs_memory_show(struct seq_file *seq, void *unused);
int p2p_debugfs_topo_show(struct seq_file *seq, void *unused);

void p2p_stats_io_issued(u32 op, u64 bytes);
void p2p_stats_io_issue_failed(u32 op);
void p2p_stats_io_complete(u32 op, blk_status_t status);
void p2p_stats_lat_add(u8 kind, u32 stage, u64 ns, u64 bytes);

#endif
