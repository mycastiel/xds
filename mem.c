// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "p2p: " fmt

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "mem.h"

#define P2P_MAX_PROBE_UDEVID 64
#define P2P_HOST_UDEVID 65

static typeof(hal_kernel_get_mem_pa_list) *get_mem_pa_list;
static typeof(hal_kernel_put_mem_pa_list) *put_mem_pa_list;

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

	return 0;

put_get_mem_pa_list:
	symbol_put(hal_kernel_get_mem_pa_list);
	return -ENODEV;
}

int p2p_mem_get_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			u64 size, struct ka_pa_wraper *wrap, u64 *wrap_nr)
{
	struct ka_mem_attr attr;
	u64 covered = 0;
	u64 cap;
	u64 got = 0;
	u32 i;
	int err;

	if (!process_id || !wrap || !wrap_nr || !size)
		return -EINVAL;
	cap = *wrap_nr;
	if (!cap)
		return -EINVAL;

	p2p_mem_fill_attr(&attr, addr, size);
	err = p2p_hal_get_pa_list(process_id, &attr, cap, &got, wrap);
	if (err)
		return err;
	if (!got || got > cap) {
		err = -EINVAL;
		goto put;
	}

	for (i = 0; i < got; i++) {
		if (!wrap[i].size)
			continue;
		if (check_add_overflow(covered, wrap[i].size, &covered)) {
			err = -EOVERFLOW;
			goto put;
		}
	}
	if (covered != size) {
		err = -EINVAL;
		goto put;
	}
	*wrap_nr = got;
	return 0;

put:
	put_mem_pa_list(process_id->devid, process_id->host_pid, &attr, got,
			wrap);
	return err;
}

void p2p_mem_put_pa_list(struct devmm_svm_process_id *process_id, u64 addr,
			 u64 size, struct ka_pa_wraper *wrap, u64 wrap_nr)
{
	struct ka_mem_attr attr;
	int err;

	p2p_mem_fill_attr(&attr, addr, size);
	err = put_mem_pa_list(process_id->devid, process_id->host_pid, &attr,
			      wrap_nr, wrap);
	if (err)
		pr_err("put_mem_pa_list err %d\n", err);
}

void p2p_mem_exit(void)
{
	symbol_put(hal_kernel_put_mem_pa_list);
	symbol_put(hal_kernel_get_mem_pa_list);
}
