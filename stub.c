// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

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
static atomic_t notify_cb_calls = ATOMIC_INIT(0);

struct stub_pin {
	struct list_head node;
	struct p2p_page_table *table;
	u64 addr;
	u64 size;
	void (*cb)(void *data);
	void *data;
};

static LIST_HEAD(stub_pins);
static DEFINE_MUTEX(stub_pins_lock);

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
module_param_cb(notify_cb_calls, &stub_atomic_param_ops, &notify_cb_calls, 0444);
MODULE_PARM_DESC(notify_cb_calls, "HAL free_callback invocations from notify_free");

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

static bool stub_ranges_overlap(u64 a, u64 a_size, u64 b, u64 b_size)
{
	return !((a + a_size) <= b || (b + b_size) <= a);
}

static void stub_pin_unlink_locked(struct stub_pin *pin)
{
	if (!list_empty(&pin->node))
		list_del_init(&pin->node);
}

static void stub_notify_overlapping(u64 va, u64 size)
{
	LIST_HEAD(hit);
	struct stub_pin *pin, *next;

	mutex_lock(&stub_pins_lock);
	list_for_each_entry_safe(pin, next, &stub_pins, node) {
		if (stub_ranges_overlap(va, size, pin->addr, pin->size))
			list_move_tail(&pin->node, &hit);
	}
	mutex_unlock(&stub_pins_lock);

	list_for_each_entry_safe(pin, next, &hit, node) {
		list_del_init(&pin->node);
		atomic_inc(&notify_cb_calls);
		pin->cb(pin->data);
		kfree(pin);
	}
}

static int stub_notify_free_set(const char *val, const struct kernel_param *kp)
{
	u64 addr;
	u64 size;
	char *endp;

	(void)kp;
	if (!val)
		return -EINVAL;
	addr = simple_strtoull(val, &endp, 0);
	if (endp == val)
		return -EINVAL;
	while (*endp == ' ' || *endp == '\t' || *endp == ',')
		endp++;
	size = simple_strtoull(endp, &endp, 0);
	if (!size)
		return -EINVAL;
	stub_notify_overlapping(addr, size);
	return 0;
}

static const struct kernel_param_ops stub_notify_free_ops = {
	.set = stub_notify_free_set,
};

module_param_cb(notify_free, &stub_notify_free_ops, NULL, 0200);
MODULE_PARM_DESC(notify_free, "Write \"<va> <size>\" to invoke matching free_callbacks");

int hal_kernel_p2p_get_pages(u64 addr, u64 size, void (*free_callback)(void *data), void *data,
			     struct p2p_page_table **page_table)
{
	struct p2p_page_table *table;
	struct stub_pin *pin;
	u64 aligned_addr;
	u64 aligned_end;
	u64 end;
	u64 pa;
	u64 i;
	int err;

	atomic_inc(&get_pa_calls);
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

	pin = kzalloc(sizeof(*pin), GFP_KERNEL);
	if (!pin) {
		kfree(table->pages_info);
		kfree(table);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&pin->node);
	pin->table = table;
	pin->addr = addr;
	pin->size = size;
	pin->cb = free_callback;
	pin->data = data;

	mutex_lock(&stub_pins_lock);
	list_add_tail(&pin->node, &stub_pins);
	mutex_unlock(&stub_pins_lock);

	*page_table = table;
	return 0;
}
EXPORT_SYMBOL_GPL(hal_kernel_p2p_get_pages);

int hal_kernel_p2p_put_pages(struct p2p_page_table *page_table)
{
	struct stub_pin *pin, *next;
	struct stub_pin *found = NULL;

	atomic_inc(&put_pa_calls);
	if (!page_table)
		return -EINVAL;

	mutex_lock(&stub_pins_lock);
	list_for_each_entry_safe(pin, next, &stub_pins, node) {
		if (pin->table == page_table) {
			stub_pin_unlink_locked(pin);
			found = pin;
			break;
		}
	}
	mutex_unlock(&stub_pins_lock);
	kfree(found);

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
	WARN_ON(!list_empty(&stub_pins));
	pr_info("xds_stub: unloaded\n");
}

module_init(stub_init);
module_exit(stub_exit);

MODULE_DESCRIPTION("Static NVMe CMB-backed device-memory stub for XDS tests");
MODULE_LICENSE("GPL");
