// SPDX-License-Identifier: GPL-2.0-only
/*
 * BPF kfuncs for pluggable memory-prefetch policies.
 *
 * A BPF program attached to bpf_prefetch_policy_hook() (mm/memory.c) uses
 * these to act on a page fault without reimplementing the kernel-internal
 * VMA/page-table plumbing. See mm/memory.c for the hook points themselves.
 */
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "internal.h"

static DEFINE_PER_CPU(struct blk_plug, bpf_prefetch_plug);

__bpf_kfunc_start_defs();

/**
 * bpf_prefetch_page_va - map a single page of the faulting VMA, if cached
 * @vmf: the vm_fault that triggered the current page fault
 * @vaddr: virtual address to prefetch; must fall within vmf->vma
 *
 * This is the helper the VMA prefetch policy uses: it mirrors what
 * do_fault_around() does for its fixed window, but for one address picked
 * by the BPF program. Pages not already resident in the page cache are
 * skipped by ->map_pages(), so this never triggers I/O on its own.
 *
 * Return: 0 on success, -EINVAL if @vaddr is outside @vmf->vma,
 * -EOPNOTSUPP if the VMA has no ->map_pages(), -EAGAIN if the page wasn't
 * ready to be mapped.
 */
__bpf_kfunc int bpf_prefetch_page_va(struct vm_fault *vmf, u64 vaddr)
{
	struct vm_area_struct *vma = vmf->vma;
	pgoff_t pgoff;
	vm_fault_t ret;

	if (!vma->vm_ops->map_pages)
		return -EOPNOTSUPP;
	if (vaddr < vma->vm_start || vaddr >= vma->vm_end) // TODO: optimize
		return -EINVAL;

	pgoff = vmf->pgoff + ((vaddr - vmf->address) >> PAGE_SHIFT);

	rcu_read_lock();
	ret = vma->vm_ops->map_pages(vmf, pgoff, pgoff);
	rcu_read_unlock();

	return ret ? -EAGAIN : 0;
}

/**
 * bpf_prefetch_page_pa - hint that a physical page is about to be used
 * @phys_addr: physical address of the page
 *
 * Helper for policies (e.g. Leap/Ladder) that reason about physical frames
 * rather than VMA-relative virtual addresses. Not used by the VMA policy.
 *
 * Return: 0 on success, -EINVAL if @phys_addr has no backing struct page.
 */
__bpf_kfunc int bpf_prefetch_page_pa(u64 phys_addr)
{
	unsigned long pfn = PHYS_PFN(phys_addr);

	if (!pfn_valid(pfn))
		return -EINVAL;

	mark_page_accessed(pfn_to_page(pfn));
	return 0;
}

/**
 * bpf_prefetch_plug_ctrl - start or finish a block-plug to batch I/O
 * @start: true to start plugging on the current CPU, false to finish it
 *
 * Lets a prefetch policy batch several block I/O requests it issues into a
 * single dispatch, the same way read_pages() does for native readahead. Not
 * used by the VMA policy, which only prefetches pages already in cache.
 *
 * Return: always 0.
 */
__bpf_kfunc int bpf_prefetch_plug_ctrl(bool start)
{
	struct blk_plug *plug = this_cpu_ptr(&bpf_prefetch_plug);

	if (start)
		blk_start_plug(plug);
	else
		blk_finish_plug(plug);
	return 0;
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(bpf_prefetch_kfunc_ids)
BTF_ID_FLAGS(func, bpf_prefetch_page_va, KF_TRUSTED_ARGS)
BTF_ID_FLAGS(func, bpf_prefetch_page_pa)
BTF_ID_FLAGS(func, bpf_prefetch_plug_ctrl)
BTF_KFUNCS_END(bpf_prefetch_kfunc_ids)

static const struct btf_kfunc_id_set bpf_prefetch_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &bpf_prefetch_kfunc_ids,
};

static int __init bpf_prefetch_kfuncs_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					  &bpf_prefetch_kfunc_set);
}
late_initcall(bpf_prefetch_kfuncs_init);
