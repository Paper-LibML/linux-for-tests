// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loader for the VMA prefetch policy. Sets the prefetch window (in pages),
 * attaches the fentry/fmod_ret programs, and periodically prints per-VMA
 * fault counts until interrupted.
 *
 * Usage: prefetch_vma [window_pages]
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "prefetch_vma.skel.h"

static volatile sig_atomic_t exiting;

static void on_sigint(int sig)
{
	exiting = 1;
}

static void dump_stats(struct prefetch_vma_bpf *skel)
{
	int fd = bpf_map__fd(skel->maps.vma_fault_count);
	__u64 key = 0, next_key, count;

	while (bpf_map_get_next_key(fd, &key, &next_key) == 0) {
		if (bpf_map_lookup_elem(fd, &next_key, &count) == 0)
			printf("vma_start=0x%llx faults=%llu\n",
			       (unsigned long long)next_key,
			       (unsigned long long)count);
		key = next_key;
	}
}

int main(int argc, char **argv)
{
	struct prefetch_vma_bpf *skel;
	__u32 zero = 0;
	__u64 window = argc > 1 ? strtoull(argv[1], NULL, 10) : 4;
	int err, map_fd;

	skel = prefetch_vma_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "failed to open/load BPF skeleton\n");
		return 1;
	}

	map_fd = bpf_map__fd(skel->maps.window_pages);
	err = bpf_map_update_elem(map_fd, &zero, &window, BPF_ANY);
	if (err) {
		fprintf(stderr, "failed to set prefetch window: %d\n", err);
		goto cleanup;
	}

	err = prefetch_vma_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}

	signal(SIGINT, on_sigint);
	printf("VMA prefetch policy activa, ventana=%llu paginas. Ctrl-C para salir.\n",
	       (unsigned long long)window);

	while (!exiting) {
		sleep(2);
		dump_stats(skel);
	}

cleanup:
	prefetch_vma_bpf__destroy(skel);
	return err != 0;
}
