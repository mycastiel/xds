// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "p2p: " fmt

#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "mem.h"

#define P2P_DEFAULT_PAGE_SIZE (4U << 10)
#define P2P_MAX_NS_UDEVS 64

static typeof(hal_kernel_get_mem_pa_list) *get_mem_pa_list;
static typeof(hal_kernel_put_mem_pa_list) *put_mem_pa_list;
static typeof(hal_kernel_get_mem_page_size) *get_mem_page_size;
static typeof(uda_devid_to_udevid) *devid_to_udevid;
static typeof(uda_get_cur_ns_dev_num) *get_cur_ns_dev_num;
static typeof(uda_get_cur_ns_udevids) *get_cur_ns_udevids;
static typeof(uda_get_host_id) *get_host_id;

static void p2p_mem_fill_attr(struct ka_mem_attr *attr, u64 addr, u64 size)
{
	attr->addr = addr;
	attr->size = size;
	attr->cp_only_flag = false;
	attr->raw_pa_flag = false;
}

static bool p2p_udevid_already(const u32 *udevids, u32 n, u32 udevid)
{
	u32 i;

	for (i = 0; i < n; i++) {
		if (udevids[i] == udevid)
			return true;
	}
	return false;
}

static u32 p2p_collect_udevids(u32 start, u32 *udevids, u32 max)
{
	u32 n = 0;
	u32 mapped;
	u32 ns_n;
	u32 ns[P2P_MAX_NS_UDEVS];
	u32 i;
	u32 host;

	if (!max)
		return 0;
	udevids[n++] = start;

	if (devid_to_udevid && !devid_to_udevid(start, &mapped) &&
	    !p2p_udevid_already(udevids, n, mapped) && n < max)
		udevids[n++] = mapped;

	if (get_cur_ns_dev_num && get_cur_ns_udevids) {
		ns_n = get_cur_ns_dev_num();
		if (ns_n > ARRAY_SIZE(ns))
			ns_n = ARRAY_SIZE(ns);
		if (ns_n && !get_cur_ns_udevids(ns, ns_n)) {
			for (i = 0; i < ns_n && n < max; i++) {
				if (!p2p_udevid_already(udevids, n, ns[i]))
					udevids[n++] = ns[i];
			}
		}
	}

	if (get_host_id) {
		host = get_host_id();
		if (!p2p_udevid_already(udevids, n, host) && n < max)
			udevids[n++] = host;
	}

	return n;
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

	devid_to_udevid = symbol_get(uda_devid_to_udevid);
	get_cur_ns_dev_num = symbol_get(uda_get_cur_ns_dev_num);
	get_cur_ns_udevids = symbol_get(uda_get_cur_ns_udevids);
	get_host_id = symbol_get(uda_get_host_id);
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
	u32 udevids[P2P_MAX_NS_UDEVS + 2];
	u64 covered = 0;
	u32 page_size;
	u32 n;
	u32 i;
	u32 out = 0;
	u64 got = 0;
	int err = -ENODEV;

	if (!process_id || !pa_list || !pa_num || !size)
		return -EINVAL;
	if (size % pa_num)
		return -EINVAL;
	page_size = size / pa_num;
	if (!page_size || !is_power_of_2(page_size))
		return -EINVAL;

	p2p_mem_fill_attr(&attr, addr, size);
	wrap = kvmalloc_array(pa_num, sizeof(*wrap), GFP_KERNEL);
	if (!wrap)
		return -ENOMEM;

	/*
	 * HAL v3 pin is keyed by udevid. process_id->devid is 0 unless the
	 * caller filled a logical id; smp_ctx_get(0) then returns -ESRCH.
	 */
	n = p2p_collect_udevids(process_id->devid, udevids,
				ARRAY_SIZE(udevids));
	for (i = 0; i < n; i++) {
		got = pa_num;
		err = get_mem_pa_list(udevids[i], process_id->host_pid, &attr,
				      &got, wrap);
		if (!err)
			break;
	}
	if (err)
		goto out;
	if (udevids[i] > U16_MAX) {
		err = -EINVAL;
		goto put;
	}
	process_id->devid = udevids[i];

	if (!got || got > pa_num) {
		err = -EINVAL;
		goto put;
	}

	/*
	 * *pa_num is in/out: capacity in, contiguous-segment count out.
	 * Unused trailing wrapers (size 0) are skipped.
	 */
	for (i = 0; i < got; i++) {
		u64 off;

		if (!wrap[i].size)
			continue;
		if (wrap[i].size % page_size) {
			err = -EINVAL;
			goto put;
		}
		if (check_add_overflow(covered, wrap[i].size, &covered)) {
			err = -EOVERFLOW;
			goto put;
		}
		for (off = 0; off < wrap[i].size; off += page_size) {
			if (out >= pa_num) {
				err = -EINVAL;
				goto put;
			}
			pa_list[out++] = wrap[i].pa + off;
		}
	}
	if (covered != size || out != pa_num) {
		err = -EINVAL;
		goto put;
	}
	err = 0;
	goto out;

put:
	put_mem_pa_list(process_id->devid, process_id->host_pid, &attr, got,
			wrap);
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
		wrap[i].size = size / pa_num;
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
	/*
	 * v3 HAL leaves get_svm_mem_page_size unset, so the exported
	 * symbol returns 0. 4 KiB is only wraper capacity; HAL still
	 * returns contiguous segments that we expand.
	 */
	if (!page_size)
		page_size = P2P_DEFAULT_PAGE_SIZE;
	return (int)page_size;
}

void p2p_mem_exit(void)
{
	if (get_host_id)
		symbol_put(uda_get_host_id);
	if (get_cur_ns_udevids)
		symbol_put(uda_get_cur_ns_udevids);
	if (get_cur_ns_dev_num)
		symbol_put(uda_get_cur_ns_dev_num);
	if (devid_to_udevid)
		symbol_put(uda_devid_to_udevid);
	symbol_put(hal_kernel_get_mem_page_size);
	symbol_put(hal_kernel_put_mem_pa_list);
	symbol_put(hal_kernel_get_mem_pa_list);
}
