# SPDX-License-Identifier: GPL-2.0
#
# Reutiliza la infraestructura de tools/testing/shared/:
#   - shared.mk  provee CFLAGS, LDLIBS, SHARED_OFILES y las reglas de build
#   - linux.o    implementa kmem_cache con malloc/pthread
#   - slab.o, find_bit.o, bitmap.o, hweight.o, vsprintf.o  de lib/
#   - xarray-shared.o  implementa xarray (requerido internamente por maple tree)

BENCH = bench_ds

.PHONY: targets clean

targets: generated/map-shift.h generated/bit-length.h generated/autoconf.h $(BENCH)

include ../shared/shared.mk

# NO agregar -I../../../include: haría que bits/local_lim.h del sistema
# encontrara los asm types del kernel causando conflictos de typedef con
# tools/include/linux/types.h (u64: unsigned long vs unsigned long long).
# Los headers del kernel que no tienen mock en tools/ se resuelven vía los
# includes relativos de los propios mocks (ej: shared/linux/maple_tree.h).

OFILES = bench_main.o $(SHARED_OFILES)

$(BENCH): $(OFILES)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

bench_main.o: bench_main.c bench.h $(SHARED_DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(BENCH) *.o radix-tree.c idr.c generated/*
