// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>

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

int hal_kernel_p2p_get_pages(u64 addr, u64 size, void (*free_callback)(void *data), void *data,
			     struct p2p_page_table **page_table)
{
	struct p2p_page_table *table;
	u64 aligned_addr;
	u64 aligned_end;
	u64 end;
	u64 pa;
	u64 i;
	int err;

	atomic_inc(&get_pa_calls);
	(void)data;
	if (!free_callback || !page_table)
		return -EINVAL;
	err = stub_validate_range(addr, size);
	if (err)
		return err;

	if (check_add_overflow(addr, size, &end) ||
	    check_add_overflow(end, (u64)STUB_PAGE_SIZE - 1, &aligned_end))
		return -EOVERFLOW;
	aligned_addr = round_down(addr, (u64)STUB_PAGE_SIZE);
	aligned_end = round_down(aligned_end, (u64)STUB_PAGE_SIZE);

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	table->page_num = (aligned_end - aligned_addr) / STUB_PAGE_SIZE;
	table->pages_info = kcalloc(table->page_num, sizeof(*table->pages_info), GFP_KERNEL);
	if (!table->pages_info) {
		kfree(table);
		return -ENOMEM;
	}
	table->version = P2P_GET_PAGE_VERSION;
	table->page_size = STUB_PAGE_SIZE;

	if (check_add_overflow((u64)base_pa, aligned_addr, &pa)) {
		kfree(table->pages_info);
		kfree(table);
		return -EOVERFLOW;
	}

	for (i = 0; i < table->page_num; i++) {
		table->pages_info[i].pa = pa;
		pa += STUB_PAGE_SIZE;
	}

	*page_table = table;
	return 0;
}
EXPORT_SYMBOL_GPL(hal_kernel_p2p_get_pages);

int hal_kernel_p2p_put_pages(struct p2p_page_table *page_table)
{
	atomic_inc(&put_pa_calls);
	if (!page_table)
		return -EINVAL;

	kfree(page_table->pages_info);
	kfree(page_table);
	return 0;
}
EXPORT_SYMBOL_GPL(hal_kernel_p2p_put_pages);

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
