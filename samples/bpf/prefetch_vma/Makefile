# SPDX-License-Identifier: GPL-2.0-only
#
# Standalone build for the VMA prefetch policy sample. Run inside the test
# VM after booting the patched kernel (needs CONFIG_DEBUG_INFO_BTF=y so
# /sys/kernel/btf/vmlinux exists, plus clang, bpftool and libbpf-dev).
#
# Usage:
#   make
#   ./.output/prefetch_vma [window_pages]

OUTPUT := .output
CLANG ?= clang
BPFTOOL ?= bpftool
ARCH := $(shell uname -m | sed 's/x86_64/x86/')
LIBBPF_CFLAGS := $(shell pkg-config --cflags libbpf 2>/dev/null)
LIBBPF_LIBS := $(shell pkg-config --libs libbpf 2>/dev/null || echo -lbpf -lelf -lz)

.PHONY: all clean

all: $(OUTPUT)/prefetch_vma

$(OUTPUT):
	mkdir -p $(OUTPUT)

$(OUTPUT)/vmlinux.h: | $(OUTPUT)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

$(OUTPUT)/prefetch_vma.bpf.o: prefetch_vma.bpf.c $(OUTPUT)/vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
		-I$(OUTPUT) $(LIBBPF_CFLAGS) -c $< -o $@

$(OUTPUT)/prefetch_vma.skel.h: $(OUTPUT)/prefetch_vma.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/prefetch_vma.o: prefetch_vma.c $(OUTPUT)/prefetch_vma.skel.h
	$(CC) -g -O2 -I$(OUTPUT) $(LIBBPF_CFLAGS) -c $< -o $@

$(OUTPUT)/prefetch_vma: $(OUTPUT)/prefetch_vma.o
	$(CC) $^ $(LIBBPF_LIBS) -o $@

clean:
	rm -rf $(OUTPUT)
