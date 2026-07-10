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
	unsigned int nsid;
	int host_pid;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	unsigned int ext_nr;
	struct fiemap_extent extents[0];
};

struct va_desc_ba {
	unsigned long *addr;
	unsigned long *size;
	int count;
};

struct read_desc_ba {
	struct va_desc_ba desc;
	unsigned int nsid;
	int file_fd;
	unsigned int ext_num;
	struct fiemap_extent extents[0];
};

struct paddr_desc {
	struct va_desc desc;
	unsigned int pa_num;
	unsigned long long data_size;
	unsigned long long pa_size;
	unsigned long long paddr[512];
};

#define IOCTL_DUMP_PA _IOW('k', 1, struct va_desc)
#define IOCTL_READ_FILE _IOWR('k', 2, struct p2p_read_param)
#define IOCTL_READ_FILE_BATCH _IOWR('k', 3, struct p2p_read_param)
#define IOCTL_DRAIN_READ _IOC(_IOC_READ, 'k', 4, 0)

#endif
