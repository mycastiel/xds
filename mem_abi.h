#ifndef P2P_MEM_ABI_H_
#define P2P_MEM_ABI_H_

#include <linux/types.h>

struct devmm_svm_process_id {
	int32_t host_pid;
	union {
		uint16_t devid;
		uint16_t vm_id;
	};
	uint16_t vfid;
	uint8_t udevid_valid;
	uint8_t reserved;
};

struct ka_pa_wraper {
	u64 pa;
	u64 size;
};

struct ka_mem_attr {
	u64 addr;
	u64 size;
	bool cp_only_flag;
	bool raw_pa_flag;
};

int hal_kernel_get_mem_pa_list(u32 devid, int tgid, struct ka_mem_attr *mem,
			       u64 *pa_num, struct ka_pa_wraper *pa_list);
int hal_kernel_put_mem_pa_list(u32 devid, int tgid, struct ka_mem_attr *mem,
			       u64 pa_num, struct ka_pa_wraper *pa_list);
u32 hal_kernel_get_mem_page_size(u32 devid, int tgid, struct ka_mem_attr *mem);

#endif
