#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include "p2p_mem_query.h"

int devmm_get_mem_pa_list(struct devmm_svm_process_id *process_id, u64 addr, u64 size, u64 *pa_list, u32 pa_num)
{
	if (addr == 0 && size <= (256 << 20)) {
		u64 base = 0x480000000ULL;
		unsigned int i;

		for (i = 0; i < pa_num; i++) {
			pa_list[i] = base;
			base += (2 << 20);
		}

		return 0;
	}
	return -ENOSYS;
}
EXPORT_SYMBOL(devmm_get_mem_pa_list);

void devmm_put_mem_pa_list(struct devmm_svm_process_id *process_id, u64 addr, u64 size, u64 *pa_list, u32 pa_num)
{
}
EXPORT_SYMBOL(devmm_put_mem_pa_list);

u32 devmm_get_mem_page_size(struct devmm_svm_process_id *process_id, u64 addr, u64 size)
{
	return (2 << 20);
}
EXPORT_SYMBOL(devmm_get_mem_page_size);

static int __init stub_init(void)
{
	return 0;
}

static void __exit stub_exit(void)
{
}

module_init(stub_init);
module_exit(stub_exit);
MODULE_LICENSE("GPL");
