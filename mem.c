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
#define P2P_MAX_PROBE_UDEVID 64
#define P2P_HOST_UDEVID 65

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

static int p2p_try_get_pa_list(u32 udevid, int tgid, struct ka_mem_attr *attr,
			       u64 pa_num, u64 *got, struct ka_pa_wraper *wrap)
{
	*got = pa_num;
	return get_mem_pa_list(udevid, tgid, attr, got, wrap);
}

static int p2p_hal_get_pa_list(struct devmm_svm_process_id *process_id,
			       struct ka_mem_attr *attr, u64 pa_num, u64 *got,
			       struct ka_pa_wraper *wrap)
{
	u32 udevid;
	int err;
	int last_err;

	if (process_id->udevid_valid)
		return p2p_try_get_pa_list(process_id->devid,
					   process_id->host_pid, attr, pa_num,
					   got, wrap);

	/*
	 * v3 HAL pin is keyed by udevid. Userspace may pass a hint in
	 * process_id->devid; if that card has no smp_ctx (-ESRCH) or the VA
	 * is not on that card (-EINVAL), walk 0..63 and host id 65.
	 */
	err = p2p_try_get_pa_list(process_id->devid, process_id->host_pid, attr,
				  pa_num, got, wrap);
	if (!err)
		goto got_it;
	if (err != -ESRCH && err != -EINVAL)
		return err;
	last_err = err;

	for (udevid = 0; udevid < P2P_MAX_PROBE_UDEVID; udevid++) {
		if (udevid == process_id->devid)
			continue;
		err = p2p_try_get_pa_list(udevid, process_id->host_pid, attr,
					  pa_num, got, wrap);
		if (!err) {
			process_id->devid = udevid;
			goto got_it;
		}
		last_err = err;
		if (err != -ESRCH && err != -EINVAL)
			return err;
	}

	if (process_id->devid != P2P_HOST_UDEVID) {
		err = p2p_try_get_pa_list(P2P_HOST_UDEVID,
					  process_id->host_pid, attr, pa_num,
					  got, wrap);
		if (!err) {
			process_id->devid = P2P_HOST_UDEVID;
			goto got_it;
		}
		last_err = err;
	}
	return last_err;

got_it:
	process_id->udevid_valid = 1;
	return 0;
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
	u64 covered = 0;
	u32 page_size;
	u32 i;
	u32 out = 0;
	u64 got = 0;
	int err;

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

	err = p2p_hal_get_pa_list(process_id, &attr, pa_num, &got, wrap);
	if (err)
		goto out;
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
	symbol_put(hal_kernel_get_mem_page_size);
	symbol_put(hal_kernel_put_mem_pa_list);
	symbol_put(hal_kernel_get_mem_pa_list);
}
