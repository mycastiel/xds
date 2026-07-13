#ifndef P2P_DEV_UAPI_H_
#define P2P_DEV_UAPI_H_

#include <linux/types.h>
#include <linux/fiemap.h>

struct p2p_iov {
	unsigned long addr;
	unsigned long size;
};

struct p2p_read_param {
	int file_fd;
	int host_pid;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	unsigned int ext_nr;
	struct fiemap_extent extents[];
};

#define P2P_TOPO_NAME_LEN 32
#define P2P_TOPO_MAX_BDEVS 4

struct topo_user_bdev {
	__u32 dev_id;
	__u32 reserved;
	__u64 size_sector;
	__u64 start_sector;
};

struct topo_user_cfg {
	char name[P2P_TOPO_NAME_LEN];
	__u32 top_dev;
	__u32 nr_devs;
	__u64 extra[2];
	struct topo_user_bdev bdevs[];
};

#define IOCTL_READ_FILE _IOWR('k', 2, struct p2p_read_param)
#define IOCTL_READ_FILE_BATCH _IOWR('k', 3, struct p2p_read_param)
#define IOCTL_DRAIN_READ _IOC(_IOC_READ, 'k', 4, 0)
#define IOCTL_ADD_TOPO _IOW('k', 5, struct topo_user_cfg)

#endif
