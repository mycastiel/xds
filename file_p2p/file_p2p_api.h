#ifndef __FILE_P2P_API_H__
#define __FILE_P2P_API_H__

#include <stdint.h>

#include "p2p_dev_uapi.h"

struct read_parameter {
	const char *file_name;
	unsigned long file_offset;
	struct p2p_iov *iov;
	unsigned int iov_nr;
	uint64_t mem_handle;
	unsigned long flags;
};

int new_p2p_fd(void);
int close_p2p_fd(int dev_fd);
int add_topo(int dev_fd, const char *dev);
int register_mem(int dev_fd, struct p2p_mem_register_param *param);
int unregister_mem(int dev_fd, const struct p2p_mem_unregister_param *param);
int read_file(int dev_fd, const struct read_parameter *param);
int drain_io(int dev_fd);

#endif
