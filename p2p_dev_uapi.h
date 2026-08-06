#ifndef P2P_DEV_UAPI_H_
#define P2P_DEV_UAPI_H_

#include <linux/types.h>
#include <linux/fiemap.h>

struct p2p_iov {
	__u64 addr;
	__u32 size;
	__u32 reserved;
};

#define P2P_IO_F_REGISTERED_MEM (1U << 0)
#define P2P_IO_F_MASK P2P_IO_F_REGISTERED_MEM

#define P2P_IO_READ 0U
#define P2P_IO_WRITE 1U

struct p2p_io_param {
	unsigned int op;
	unsigned int flags;
	int file_fd;
	int host_pid;
	__u64 mem_handle;
	__u64 user_data;          /* echo'd in completion event */
	__u64 iov;                /* userspace pointer to struct p2p_iov[] */
	unsigned int iov_nr;
	unsigned int ext_nr;
	struct fiemap_extent extents[];
};

struct p2p_io_event {
	__u64 user_data;
	__s64 res;                /* 0 on success or -errno */
	__u64 reserved[2];        /* pad to 32B; matches NDS nds_io_event */
};

struct p2p_getevents_param {
	__s32 min_nr;
	__s32 max_nr;
	__s64 timeout_ns;       /* <0 infinite, 0 nonblock, >0 relative */
	__u64 events;           /* userspace pointer to struct p2p_io_event[] */
};

struct p2p_mem_register_param {
	__u64 addr;
	__u64 size;
	__u64 reserved;
	__u64 mem_handle;
};

struct p2p_mem_unregister_param {
	__u64 mem_handle;
	__u64 reserved;
};

#define P2P_TOPO_NAME_LEN 32
#define P2P_TOPO_MAX_BDEVS 4

struct topo_user_bdev {
	__u32 dev_id;
	__u32 reserved;
	/* [start_sector, start_sector + size_sector) */
	__u64 size_sector;
	__u64 start_sector;
};

struct topo_user_cfg {
	/*
	 * top_dev is the block device hosting the file system. For an nvme
	 * topology, it may be a whole namespace or a partition. Components may
	 * also be partitions, but their whole devices must be direct,
	 * non-multipath NVMe namespaces.
	 */
	char name[P2P_TOPO_NAME_LEN];
	__u32 top_dev;
	__u32 nr_devs;
	__u64 extra[2];
	struct topo_user_bdev bdevs[];
};

#define IOCTL_ADD_TOPO _IOW('k', 1, struct topo_user_cfg)
#define IOCTL_RW_FILE _IOWR('k', 2, struct p2p_io_param)
/*
 * Drain snapshots I/O contexts already published by read/write ioctls. An I/O
 * or another drain that overlaps this ioctl need not be covered by its wait.
 */
#define IOCTL_DRAIN_IO _IOC(_IOC_READ, 'k', 3, 0)
#define IOCTL_REGISTER_MEM _IOWR('k', 4, struct p2p_mem_register_param)
#define IOCTL_UNREGISTER_MEM _IOW('k', 5, struct p2p_mem_unregister_param)
#define IOCTL_GET_IO_EVENTS _IOWR('k', 6, struct p2p_getevents_param)

#endif
