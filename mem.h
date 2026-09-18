#ifndef P2P_MEM_H_
#define P2P_MEM_H_

#include "mem_abi.h"

int p2p_mem_get_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			u64 size, struct ka_pa_wraper *wrap, u64 *wrap_nr);
void p2p_mem_put_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			 u64 size, struct ka_pa_wraper *wrap, u64 wrap_nr);

int p2p_mem_init(void);
void p2p_mem_exit(void);

#endif
