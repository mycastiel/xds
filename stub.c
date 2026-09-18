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

int hal_kernel_get_mem_pa_list(u32 devid, int tgid, struct ka_mem_attr *mem,
			       u64 *pa_num, struct ka_pa_wraper *pa_list)
{
	u64 expected_pa_num;
	u64 pa;
	int err;

	atomic_inc(&get_pa_calls);
	(void)devid;
	(void)tgid;
	if (!mem || !pa_num || !pa_list)
		return -EINVAL;
	err = stub_validate_range(mem->addr, mem->size);
	if (err)
		return err;
	if (!IS_ALIGNED(mem->addr, STUB_PAGE_SIZE) ||
	    !IS_ALIGNED(mem->size, STUB_PAGE_SIZE))
		return -EINVAL;

	expected_pa_num = mem->size / STUB_PAGE_SIZE;
	if (!expected_pa_num || expected_pa_num > U32_MAX || *pa_num < 1)
		return -EINVAL;
	if (check_add_overflow((u64)base_pa, mem->addr, &pa))
		return -EOVERFLOW;

	/*
	 * Same contract as the real HAL: one contiguous wraper covering the
	 * whole range, not one entry per page. *pa_num is capacity in /
	 * segment count out.
	 */
	pa_list[0].pa = pa;
	pa_list[0].size = mem->size;
	*pa_num = 1;
	return 0;
}
EXPORT_SYMBOL_GPL(hal_kernel_get_mem_pa_list);

int hal_kernel_put_mem_pa_list(u32 devid, int tgid, struct ka_mem_attr *mem,
			       u64 pa_num, struct ka_pa_wraper *pa_list)
{
	atomic_inc(&put_pa_calls);
	(void)devid;
	(void)tgid;
	(void)mem;
	(void)pa_num;
	(void)pa_list;
	return 0;
}
EXPORT_SYMBOL_GPL(hal_kernel_put_mem_pa_list);

u32 hal_kernel_get_mem_page_size(u32 devid, int tgid, struct ka_mem_attr *mem)
{
	(void)devid;
	(void)tgid;
	if (!mem || stub_validate_range(mem->addr, mem->size))
		return 0;

	return STUB_PAGE_SIZE;
}
EXPORT_SYMBOL_GPL(hal_kernel_get_mem_page_size);

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
