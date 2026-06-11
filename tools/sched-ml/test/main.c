/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Userspace validation harness for the sched-ml decision tree.
 *
 * What it checks:
 *   1. Fixed-point arithmetic correctness (unit tests).
 *   2. dtree_infer() output against manually computed expected values.
 *   3. If a trace CSV is supplied (--trace), compares the tree's predictions
 *      against the labels in the file and reports accuracy.
 *
 * Trace CSV format (one row per scheduling event):
 *   label, feat0, feat1, ..., featN
 *   where label = task index that should have been picked.
 *
 * Build:
 *   make -C tools/sched-ml/test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Pull in the headers from the parent directory */
#include "../include/fixpoint.h"
#include "../include/dtree.h"
#include "../model/model_weights.h"

/* -------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...) \
	do { \
		if (cond) { \
			g_pass++; \
		} else { \
			fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
				__FILE__, __LINE__, ##__VA_ARGS__); \
			g_fail++; \
		} \
	} while (0)

/* -------------------------------------------------------------------------
 * Fixed-point unit tests
 * ---------------------------------------------------------------------- */

static void test_fp_conversions(void)
{
	fp_t a = fp_from_int(5);
	CHECK(fp_to_int(a) == 5, "int round-trip: got %lld", (long long)fp_to_int(a));

	fp_t b = fp_from_int(-3);
	CHECK(fp_to_int(b) == -3, "negative round-trip: got %lld", (long long)fp_to_int(b));

	fp_t zero = fp_from_int(0);
	CHECK(fp_to_int(zero) == 0, "zero round-trip");
}

static void test_fp_arithmetic(void)
{
	fp_t two   = fp_from_int(2);
	fp_t three = fp_from_int(3);

	CHECK(fp_to_int(fp_add(two, three)) == 5,  "2 + 3 = 5");
	CHECK(fp_to_int(fp_sub(three, two)) == 1,  "3 - 2 = 1");
	CHECK(fp_to_int(fp_mul(two, three)) == 6,  "2 * 3 = 6");
	CHECK(fp_to_int(fp_div(three, two)) == 1,  "3 / 2 = 1 (truncated)");

	/* Fractional precision: 1/2 should round-trip via fp_to_int_round */
	fp_t half = fp_div(fp_from_int(1), fp_from_int(2));
	CHECK(fp_to_int_round(half) == 1, "round(0.5) = 1, got %lld",
	      (long long)fp_to_int_round(half));

	/* ReLU */
	CHECK(fp_relu(fp_from_int(-7)) == 0,           "relu(-7) = 0");
	CHECK(fp_relu(fp_from_int(4))  == fp_from_int(4), "relu(4) = 4");

	/* Absolute value */
	CHECK(fp_abs(fp_from_int(-9)) == fp_from_int(9), "abs(-9) = 9");
}

static void test_fp_comparisons(void)
{
	fp_t a = fp_from_int(3);
	fp_t b = fp_from_int(7);

	CHECK(fp_lt(a, b) == FP_ONE, "3 < 7");
	CHECK(fp_lt(b, a) == 0,      "7 < 3 is false");
	CHECK(fp_gt(b, a) == FP_ONE, "7 > 3");
	CHECK(fp_eq(a, a) == FP_ONE, "3 == 3");
	CHECK(fp_eq(a, b) == 0,      "3 == 7 is false");
}

/* -------------------------------------------------------------------------
 * Decision tree unit tests
 * ---------------------------------------------------------------------- */

/*
 * Build a minimal 3-node tree and verify inference manually.
 *
 * Tree:
 *   root: features[21] < 1000 ?
 *     yes → leaf (value=0)   task 0 has smaller vruntime
 *     no  → leaf (value=1)   task 1 has smaller vruntime
 */
static void test_dtree_basic(void)
{
	static const struct dtree_node nodes[] = {
		{ .left=1, .right=2, .feature=21, .threshold=1000ULL, .value=0 },
		{ .left=-1, .right=-1, .feature=-1, .threshold=0,     .value=0 },
		{ .left=-1, .right=-1, .feature=-1, .threshold=0,     .value=1 },
	};

	/* Feature vector: pid0=1, vruntime0=500, deadline0=1000,
	 *                 pid1=2, vruntime1=2000, deadline1=3000 */
	u64 features[26] = {0};
	features[DTREE_HDR_SLOTS + 0] = 1;    /* pid      task 0 */
	features[DTREE_HDR_SLOTS + 1] = 500;  /* vruntime task 0 */
	features[DTREE_HDR_SLOTS + 2] = 1000; /* deadline task 0 */
	features[DTREE_HDR_SLOTS + 3] = 2;    /* pid      task 1 */
	features[DTREE_HDR_SLOTS + 4] = 2000; /* vruntime task 1 */
	features[DTREE_HDR_SLOTS + 5] = 3000; /* deadline task 1 */

	u64 idx = dtree_infer(nodes, 3, features);
	CHECK(idx == 0, "task 0 has smaller vruntime → index 0, got %llu", (unsigned long long)idx);

	/* Swap: task 1 now has smaller vruntime */
	features[DTREE_HDR_SLOTS + 1] = 5000;
	features[DTREE_HDR_SLOTS + 4] = 200;

	idx = dtree_infer(nodes, 3, features);
	CHECK(idx == 1, "task 1 has smaller vruntime → index 1, got %llu", (unsigned long long)idx);
}

static void test_dtree_hook(void)
{
	u64 features[26] = {0};
	features[DTREE_HDR_SLOTS + 1] = 100;  /* vruntime task 0 */
	features[DTREE_HDR_SLOTS + 4] = 900;  /* vruntime task 1 */

	u64 out = 0xDEAD;
	int rc = dtree_hook(&THE_MODEL, features, &out);
	/* Just verify it returns without crashing; exact value depends on model */
	CHECK(rc == 0, "dtree_hook returned %d", rc);
	CHECK(out != 0xDEAD, "dtree_hook wrote output, got %llu", (unsigned long long)out);
}

/* -------------------------------------------------------------------------
 * Trace-file validation
 * ---------------------------------------------------------------------- */

#define MAX_FEATURES 256

static int run_trace(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "Cannot open trace: %s\n", path);
		return 1;
	}

	u64  features[MAX_FEATURES];
	long correct = 0, total = 0;
	char line[4096];

	/* Skip optional header line if it starts with a letter */
	if (fgets(line, sizeof(line), f)) {
		if (line[0] >= 'a' && line[0] <= 'z')
			/* header — skip it */ ;
		else
			rewind(f);
	}

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '\n' || line[0] == '#')
			continue;

		memset(features, 0, sizeof(features));

		/* Parse: label, feat0, feat1, ... */
		char *tok = strtok(line, ",");
		if (!tok) continue;
		u64 label = (u64)strtoull(tok, NULL, 10);

		u32 idx = 0;
		while ((tok = strtok(NULL, ",\n")) && idx < MAX_FEATURES)
			features[idx++] = (u64)strtoull(tok, NULL, 10);

		u64 predicted = dtree_infer(THE_MODEL.nodes, THE_MODEL.n_nodes, features);
		if (predicted == label)
			correct++;
		total++;
	}

	fclose(f);

	if (total == 0) {
		fprintf(stderr, "Trace file had no valid rows.\n");
		return 1;
	}

	double acc = 100.0 * correct / total;
	printf("Trace accuracy: %ld / %ld = %.2f%%\n", correct, total, acc);
	return 0;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	printf("=== sched-ml userspace validation ===\n\n");

	printf("-- Fixed-point conversions --\n");
	test_fp_conversions();

	printf("-- Fixed-point arithmetic --\n");
	test_fp_arithmetic();

	printf("-- Fixed-point comparisons --\n");
	test_fp_comparisons();

	printf("-- Decision tree (manual) --\n");
	test_dtree_basic();

	printf("-- Decision tree (loaded model) --\n");
	test_dtree_hook();

	printf("\nResults: %d passed, %d failed\n\n", g_pass, g_fail);

	/* Optional trace file */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
			run_trace(argv[i + 1]);
			i++;
		}
	}

	return g_fail > 0 ? 1 : 0;
}
