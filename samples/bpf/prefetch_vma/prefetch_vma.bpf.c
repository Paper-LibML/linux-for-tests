// SPDX-License-Identifier: GPL-2.0-only
/*
 * VMA prefetch policy: on a page fault, prefetches a configurable window
 * of pages around the faulting address within the same VMA, replacing the
 * kernel's native fault-around for VMAs this policy is attached to.
 *
 * Attaches to the two hook points added in mm/memory.c and calls the
 * kfunc exported by mm/bpf_prefetch.c to do the actual page mapping.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

#define PAGE_SZ 4096UL

extern int bpf_prefetch_page_va(struct vm_fault *vmf, __u64 vaddr) __ksym;

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} window_pages SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, __u64);   /* vma->vm_start, used as a per-VMA identity */
	__type(value, __u64); /* fault count */
} vma_fault_count SEC(".maps");

struct prefetch_ctx {
	struct vm_fault *vmf;
	__u64 start;
};

static long prefetch_cb(__u32 i, void *ctx)
{
	struct prefetch_ctx *pc = ctx;

	bpf_prefetch_page_va(pc->vmf, pc->start + (__u64)i * PAGE_SZ);
	return 0;
}

SEC("fentry/bpf_prefetch_stats_hook")
int BPF_PROG(prefetch_vma_stats, struct vm_fault *vmf)
{
	__u64 key = (__u64)vmf->vma->vm_start;
	__u64 *cnt, init = 1;

	cnt = bpf_map_lookup_elem(&vma_fault_count, &key);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);
	else
		bpf_map_update_elem(&vma_fault_count, &key, &init, BPF_ANY);
	return 0;
}

SEC("fmod_ret/bpf_prefetch_policy_hook")
int BPF_PROG(prefetch_vma_policy, struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct prefetch_ctx pc;
	__u32 zero = 0, nr;
	__u64 *winp, win, addr, start, end;

	winp = bpf_map_lookup_elem(&window_pages, &zero);
	win = winp ? *winp : 4;
	if (win == 0)
		return 0;

	addr = vmf->address;
	start = addr > win * PAGE_SZ ? addr - win * PAGE_SZ : (__u64)vma->vm_start;
	if (start < (__u64)vma->vm_start)
		start = (__u64)vma->vm_start;

	end = addr + win * PAGE_SZ;
	if (end > (__u64)vma->vm_end)
		end = (__u64)vma->vm_end;

	if (end <= start)
		return 0;

	nr = (end - start) / PAGE_SZ;
	pc.vmf = vmf;
	pc.start = start;
	bpf_loop(nr, prefetch_cb, &pc, 0);

	/* Overrides bpf_prefetch_policy_hook()'s return value (see
	 * ALLOW_ERROR_INJECTION(..., TRUE) in mm/memory.c): true tells
	 * do_read_fault() to skip the native do_fault_around().
	 */
	return 1;
}
