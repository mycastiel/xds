// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "p2p: " fmt

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "mem.h"

static typeof(hal_kernel_get_mem_pa_list) *get_mem_pa_list;
static typeof(hal_kernel_put_mem_pa_list) *put_mem_pa_list;
static typeof(hal_kernel_get_mem_page_size) *get_mem_page_size;

static void p2p_mem_fill_attr(struct ka_mem_attr *attr, u64 addr, u64 size)
{
	attr->addr = addr;
	attr->size = size;
	attr->cp_only_flag = false;
	attr->raw_pa_flag = false;
}

int p2p_mem_init(void)
{
	get_mem_pa_list = symbol_get(hal_kernel_get_mem_pa_list);
	if (!get_mem_pa_list) {
		pr_err("required symbol hal_kernel_get_mem_pa_list is unavailable\n");
		return -ENODEV;
	}

	put_mem_pa_list = symbol_get(hal_kernel_put_mem_pa_list);
	if (!put_mem_pa_list) {
		pr_err("required symbol hal_kernel_put_mem_pa_list is unavailable\n");
		goto put_get_mem_pa_list;
	}

	get_mem_page_size = symbol_get(hal_kernel_get_mem_page_size);
	if (!get_mem_page_size) {
		pr_err("required symbol hal_kernel_get_mem_page_size is unavailable\n");
		goto put_put_mem_pa_list;
	}

	return 0;

put_put_mem_pa_list:
	symbol_put(hal_kernel_put_mem_pa_list);
put_get_mem_pa_list:
	symbol_put(hal_kernel_get_mem_pa_list);
	return -ENODEV;
}

int p2p_mem_get_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			u64 size, u64 *pa_list, u32 pa_num)
{
	struct ka_mem_attr attr;
	struct ka_pa_wraper *wrap;
	u64 got = pa_num;
	u32 i;
	int err;

	p2p_mem_fill_attr(&attr, addr, size);
	wrap = kvmalloc_array(pa_num, sizeof(*wrap), GFP_KERNEL);
	if (!wrap)
		return -ENOMEM;

	err = get_mem_pa_list(process_id->devid, process_id->host_pid, &attr,
			      &got, wrap);
	if (err)
		goto out;
	if (got != pa_num) {
		put_mem_pa_list(process_id->devid, process_id->host_pid, &attr,
				got, wrap);
		err = -EINVAL;
		goto out;
	}

	for (i = 0; i < pa_num; i++)
		pa_list[i] = wrap[i].pa;

out:
	kvfree(wrap);
	return err;
}

void p2p_mem_put_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			 u64 size, u64 *pa_list, u32 pa_num)
{
	struct ka_mem_attr attr;
	struct ka_pa_wraper *wrap;
	u32 i;
	int err;

	p2p_mem_fill_attr(&attr, addr, size);
	wrap = kvmalloc_array(pa_num, sizeof(*wrap), GFP_KERNEL);
	if (!wrap) {
		pr_err("put_mem_pa_list wrap alloc failed, num %u\n", pa_num);
		return;
	}

	for (i = 0; i < pa_num; i++) {
		wrap[i].pa = pa_list[i];
		wrap[i].size = 0;
	}

	err = put_mem_pa_list(process_id->devid, process_id->host_pid, &attr,
			      pa_num, wrap);
	if (err)
		pr_err("put_mem_pa_list err %d\n", err);
	kvfree(wrap);
}

int p2p_mem_get_page_size(struct devmm_svm_process_id *process_id, u64 addr,
			  u64 size)
{
	struct ka_mem_attr attr;
	u32 page_size;

	p2p_mem_fill_attr(&attr, addr, size);
	page_size = get_mem_page_size(process_id->devid, process_id->host_pid,
				      &attr);
	return page_size ? (int)page_size : -EINVAL;
}

void p2p_mem_exit(void)
{
	symbol_put(hal_kernel_get_mem_page_size);
	symbol_put(hal_kernel_put_mem_pa_list);
	symbol_put(hal_kernel_get_mem_pa_list);
}
