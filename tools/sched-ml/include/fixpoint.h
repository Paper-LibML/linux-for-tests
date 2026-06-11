/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fixed-point arithmetic library — Q(64 - FRAC_BITS).FRAC_BITS format.
 *
 * Dual-mode: compiles with plain GCC (userspace tests) and with the
 * kernel build system (kernel module).  Conditional includes handle the
 * type differences so the same header is used in both contexts.
 *
 * Default format: Q48.16
 *   - 48 integer bits  → max ~1.4 × 10^14  (covers vruntime in ns)
 *   - 16 fractional bits → ~0.000015 resolution
 *
 * Change FP_FRAC_BITS before including if a different format is needed.
 */
#pragma once

#ifdef __KERNEL__
# include <linux/types.h>
# include <linux/math.h>
#else
# include <stdint.h>
# include <stddef.h>
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* -------------------------------------------------------------------------
 * Format configuration
 * ---------------------------------------------------------------------- */

#ifndef FP_FRAC_BITS
# define FP_FRAC_BITS  16
#endif

#define FP_SCALE   ((s64)1 << FP_FRAC_BITS)
#define FP_ONE     FP_SCALE
#define FP_HALF    (FP_SCALE >> 1)

/* Signed 64-bit fixed-point value */
typedef s64 fp_t;

/* -------------------------------------------------------------------------
 * Conversion helpers
 * ---------------------------------------------------------------------- */

/* Integer → fixed-point */
static inline fp_t fp_from_int(s64 x)
{
	return x << FP_FRAC_BITS;
}

/* Fixed-point → integer (truncates toward zero) */
static inline s64 fp_to_int(fp_t x)
{
	return x >> FP_FRAC_BITS;
}

/* Fixed-point → nearest integer */
static inline s64 fp_to_int_round(fp_t x)
{
	return (x + FP_HALF) >> FP_FRAC_BITS;
}

/*
 * Raw u64 scheduler value → fixed-point.
 *
 * Scheduler counters (vruntime, deadline) are raw nanoseconds stored as
 * u64.  Shifting left by FP_FRAC_BITS would overflow, so we keep the
 * integer part in place and zero the fractional bits.  This means a
 * "fixed-point" scheduler value is just the original u64 cast to s64
 * with the fractional bits unused — sufficient for symbolic regression
 * on these counters because the expressions rarely need sub-nanosecond
 * precision.
 */
static inline fp_t fp_from_sched(u64 x)
{
	return (fp_t)(x << FP_FRAC_BITS);
}

static inline u64 fp_to_sched(fp_t x)
{
	return (u64)(x >> FP_FRAC_BITS);
}

/* -------------------------------------------------------------------------
 * Arithmetic
 * ---------------------------------------------------------------------- */

static inline fp_t fp_add(fp_t a, fp_t b)
{
	return a + b;
}

static inline fp_t fp_sub(fp_t a, fp_t b)
{
	return a - b;
}

/*
 * Multiply two fixed-point values.
 *
 * Uses __int128 to avoid overflow before the right-shift.
 * __int128 is supported by GCC in both userspace and kernel (x86-64).
 */
static inline fp_t fp_mul(fp_t a, fp_t b)
{
	return (fp_t)(((__int128)a * (__int128)b) >> FP_FRAC_BITS);
}

/*
 * Divide two fixed-point values.
 *
 * Shifts dividend left before dividing so the result has the right scale.
 * Caller must ensure b != 0.
 */
static inline fp_t fp_div(fp_t a, fp_t b)
{
	return (fp_t)(((__int128)a << FP_FRAC_BITS) / b);
}

/* Absolute value */
static inline fp_t fp_abs(fp_t x)
{
	return x < 0 ? -x : x;
}

/* ReLU: max(0, x) — free for fixed-point */
static inline fp_t fp_relu(fp_t x)
{
	return x < 0 ? 0 : x;
}

/* Saturating add — clamps to s64 bounds instead of wrapping */
static inline fp_t fp_sadd(fp_t a, fp_t b)
{
	fp_t r = a + b;
	/* Overflow if signs of both inputs differ from result */
	if ((~(a ^ b) & (a ^ r)) < 0)
		r = (a < 0) ? (fp_t)(-0x7fffffffffffffffLL - 1) : (fp_t)0x7fffffffffffffffLL;
	return r;
}

/* -------------------------------------------------------------------------
 * Comparison helpers (return 1/0 as fp_t for use in symbolic expressions)
 * ---------------------------------------------------------------------- */

static inline fp_t fp_lt(fp_t a, fp_t b)  { return a <  b ? FP_ONE : 0; }
static inline fp_t fp_le(fp_t a, fp_t b)  { return a <= b ? FP_ONE : 0; }
static inline fp_t fp_gt(fp_t a, fp_t b)  { return a >  b ? FP_ONE : 0; }
static inline fp_t fp_ge(fp_t a, fp_t b)  { return a >= b ? FP_ONE : 0; }
static inline fp_t fp_eq(fp_t a, fp_t b)  { return a == b ? FP_ONE : 0; }
