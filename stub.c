// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>

#include "mem_abi.h"

#define STUB_ADDR_SHIFT 12
#define STUB_PAGE_SIZE SZ_2M
#define STUB_DEFAULT_MAX_PAGES ((128U << 20) >> STUB_ADDR_SHIFT)

static unsigned long long base_pa;
module_param(base_pa, ullong, 0444);
MODULE_PARM_DESC(base_pa, "Physical base address of the NVMe CMB");

static unsigned int max_pages = STUB_DEFAULT_MAX_PAGES;
module_param(max_pages, uint, 0444);
MODULE_PARM_DESC(max_pages, "CMB-backed VA window size in 4 KiB pages");

static atomic_t get_pa_calls = ATOMIC_INIT(0);
static atomic_t put_pa_calls = ATOMIC_INIT(0);

static int stub_param_get_atomic(char *buffer, const struct kernel_param *kp)
{
	atomic_t *value = kp->arg;

	return scnprintf(buffer, PAGE_SIZE, "%d\n", atomic_read(value));
}

static const struct kernel_param_ops stub_atomic_param_ops = {
	.get = stub_param_get_atomic,
};

module_param_cb(get_pa_calls, &stub_atomic_param_ops, &get_pa_calls, 0444);
MODULE_PARM_DESC(get_pa_calls, "Successful and failed PA-list get calls");
module_param_cb(put_pa_calls, &stub_atomic_param_ops, &put_pa_calls, 0444);
MODULE_PARM_DESC(put_pa_calls, "PA-list put calls");

static u64 stub_va_size(void)
{
	return (u64)max_pages << STUB_ADDR_SHIFT;
}

static int stub_validate_range(u64 addr, u64 size)
{
	u64 va_size = stub_va_size();

	if (!size || addr >= va_size || size > va_size - addr)
		return -ERANGE;

	return 0;
}

int devmm_get_mem_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			  u64 size, u64 *pa_list, u32 pa_num)
{
	u64 expected_pa_num;
	u64 pa;
	unsigned int i;
	int err;

	atomic_inc(&get_pa_calls);
	(void)process_id;
	if (!pa_list)
		return -EINVAL;
	err = stub_validate_range(addr, size);
	if (err)
		return err;
	if (!IS_ALIGNED(addr, STUB_PAGE_SIZE) ||
	    !IS_ALIGNED(size, STUB_PAGE_SIZE))
		return -EINVAL;

	expected_pa_num = size / STUB_PAGE_SIZE;
	if (!expected_pa_num || expected_pa_num > U32_MAX ||
	    pa_num != expected_pa_num)
		return -EINVAL;
	if (check_add_overflow((u64)base_pa, addr, &pa))
		return -EOVERFLOW;

	for (i = 0; i < pa_num; i++) {
		pa_list[i] = pa;
		pa += STUB_PAGE_SIZE;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(devmm_get_mem_pa_list);

void devmm_put_mem_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			   u64 size, u64 *pa_list, u32 pa_num)
{
	atomic_inc(&put_pa_calls);
	(void)process_id;
	(void)addr;
	(void)size;
	(void)pa_list;
	(void)pa_num;
}
EXPORT_SYMBOL_GPL(devmm_put_mem_pa_list);

int devmm_get_mem_page_size(struct devmm_svm_process_id *process_id, u64 addr,
			    u64 size)
{
	(void)process_id;
	if (stub_validate_range(addr, size))
		return -ERANGE;

	return STUB_PAGE_SIZE;
}
EXPORT_SYMBOL_GPL(devmm_get_mem_page_size);

static int __init stub_init(void)
{
	u64 pa_end;
	u64 va_size = stub_va_size();

	if (!base_pa || !va_size || !IS_ALIGNED(base_pa, STUB_PAGE_SIZE) ||
	    !IS_ALIGNED(va_size, STUB_PAGE_SIZE) ||
	    check_add_overflow((u64)base_pa, va_size, &pa_end)) {
		pr_err("xds_stub: invalid base_pa 0x%llx max_pages %u\n",
		       base_pa, max_pages);
		return -EINVAL;
	}

	pr_info("xds_stub: VA [0,0x%llx) -> PA [0x%llx,0x%llx), page size 0x%x\n",
		va_size, base_pa, pa_end, STUB_PAGE_SIZE);
	return 0;
}

static void __exit stub_exit(void)
{
	pr_info("xds_stub: unloaded\n");
}

module_init(stub_init);
module_exit(stub_exit);

MODULE_DESCRIPTION("Static NVMe CMB-backed device-memory stub for XDS tests");
MODULE_LICENSE("GPL");
