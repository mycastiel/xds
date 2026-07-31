#ifndef __NDS_API_H__
#define __NDS_API_H__

#include <stdint.h>
#include <sys/types.h>

#define NDS_API_VERSION 1u

struct nds_io_ctx;

struct nds_fs_desc {
	int32_t *fs_fd;
	uint32_t fs_fd_cnt;
	uint32_t reserved;
};

struct nds_init_param {
	uint32_t flags;
	/* File fd identifying the top device (regular file or block device). */
	union {
		uint64_t _data[4];
		struct nds_fs_desc desc;
	};
	uint32_t version;
};

struct nds_io_ctx_param {
	uint32_t max_io_nr;
	uint32_t flags;
	uint64_t reserved;
};

#define NDS_IO_OBJ_FD   1u
#define NDS_IO_OBJ_ID   2u

struct nds_io_obj {
	uint32_t type;
	uint32_t reserved1;
	union {
		uint64_t _data;
		int32_t fd;
	};
	uint64_t reserved2;
};

#define NDS_IO_OP_PREAD    0u
#define NDS_IO_OP_PWRITE   1u
#define NDS_IO_OP_PREADV   2u
#define NDS_IO_OP_PWRITEV  3u

#define NDS_IO_F_REGISTERED_MEM (1u << 0)
#define NDS_IO_F_MASK           NDS_IO_F_REGISTERED_MEM

struct nds_io_cb {
	uint32_t opcode;
	uint32_t rw_flags;
	struct nds_io_obj obj;
	uint64_t offset;
	uint64_t user_data;
	uint64_t buf_addr;
	uint32_t buf_len;
	/*
	 * Process owning one-shot HBM VAs. 0 = current process (same as
	 * file_p2p rw_file). Must be 0 with NDS_IO_F_REGISTERED_MEM.
	 */
	int32_t host_pid;
	uint64_t reserved;
};

struct nds_io_event {
	uint64_t user_data;
	int64_t res;
	uint32_t opcode;
	uint32_t reserved1;
	uint64_t reserved2[2];
};

int nds_init(struct nds_init_param *param);
int nds_exit(void);

int nds_register_mem(void *addr, uint64_t size, int flags);
int nds_unregister_mem(void *addr);

int nds_io_new_ctx(const struct nds_io_ctx_param *param,
		   struct nds_io_ctx **ctx);
int nds_io_destroy_ctx(struct nds_io_ctx *ctx);

int nds_io_submit(struct nds_io_ctx *ctx, unsigned int nr,
		  struct nds_io_cb *iocb);
int nds_io_getevents(struct nds_io_ctx *ctx, unsigned int min_nr,
		     unsigned int nr, struct nds_io_event *events,
		     struct timespec *timeout);

#endif
