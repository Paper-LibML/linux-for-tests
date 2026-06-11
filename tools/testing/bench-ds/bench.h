/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * bench.h — Capa de compatibilidad: debe incluirse ANTES de cualquier
 * header o fuente del kernel.  El orden importa porque los mocks tienen
 * que estar definidos antes de que los #include del kernel los usen.
 *
 * Cadena de resolución de headers (ver README.md para el detalle completo):
 *
 *   #include "shared.h"          →  tools/testing/shared/shared.h
 *                                     (types, bug, kernel, gfp, rcupdate)
 *   #include <linux/maple_tree.h> →  shared/linux/maple_tree.h  (mock)
 *                                       → include/linux/maple_tree.h (real)
 *   #include <linux/rbtree.h>    →  tools/include/linux/rbtree.h
 *   #include <linux/list.h>      →  tools/include/linux/list.h
 */
#ifndef _BENCH_H
#define _BENCH_H

/* C estándar primero */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>

/*
 * shared.h viene de tools/testing/shared/ (vía -I../shared en el Makefile).
 * Define: types, bug.h, kernel.h, bitops.h, gfp.h, rcupdate.h
 * y los no-ops de módulo (module_init, MODULE_AUTHOR, etc.)
 */
#include "shared.h"
/* Provee #define __init (vacío): maple_tree_init() lo usa en su firma */
#include <linux/init.h>

/* Estructuras de datos que vamos a comparar */
#include <linux/maple_tree.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/slab.h>

/* ---- Utilidades de temporización ---- */

#define NSEC_PER_SEC 1000000000ULL

static inline unsigned long long bench_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * NSEC_PER_SEC
		+ (unsigned long long)ts.tv_nsec;
}

#endif /* _BENCH_H */
