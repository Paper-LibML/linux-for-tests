/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Decision tree inference — dual-mode (userspace / kernel module).
 *
 * The tree is stored as a flat array of nodes (same layout that sklearn
 * exports via tree_.children_left / _right / feature / threshold / value).
 *
 * Feature vector layout (matches kernel/sched/fair.c):
 *
 *   features[0 .. DTREE_HDR_SLOTS - 1]          global / header slots
 *   features[DTREE_HDR_SLOTS + i*DTREE_TASK_FIELDS + 0]  pid   of task i
 *   features[DTREE_HDR_SLOTS + i*DTREE_TASK_FIELDS + 1]  vruntime of task i
 *
 * All comparisons use raw u64 arithmetic — no fixed-point needed for the
 * tree itself.  Thresholds are quantized from sklearn float64 by the
 * quantizer script (safe because vruntime values fit in float64 exactly).
 */
#pragma once

#ifdef __KERNEL__
# include <linux/types.h>
#else
# include <stdint.h>
typedef uint64_t u64;
typedef int32_t  s32;
#endif

/* -------------------------------------------------------------------------
 * Feature vector constants — must match fair.c
 * ---------------------------------------------------------------------- */

#define DTREE_HDR_SLOTS    20   /* global header fields (currently reserved)  */
#define DTREE_TASK_FIELDS   3   /* fields per task: pid, vruntime, deadline   */

static inline u64 dtree_feat_pid(const u64 *features, u32 task_idx)
{
	return features[DTREE_HDR_SLOTS + task_idx * DTREE_TASK_FIELDS + 0];
}

static inline u64 dtree_feat_vruntime(const u64 *features, u32 task_idx)
{
	return features[DTREE_HDR_SLOTS + task_idx * DTREE_TASK_FIELDS + 1];
}

static inline u64 dtree_feat_deadline(const u64 *features, u32 task_idx)
{
	return features[DTREE_HDR_SLOTS + task_idx * DTREE_TASK_FIELDS + 2];
}

/* -------------------------------------------------------------------------
 * Tree node
 * ---------------------------------------------------------------------- */

/*
 * DTREE_LEAF_SENTINEL matches sklearn's TREE_LEAF (-1) for children.
 * Stored as s32 to allow -1; cast-to-u32 gives 0xFFFFFFFF which is
 * intentionally outside any valid node index.
 */
#define DTREE_LEAF_SENTINEL  (-1)

struct dtree_node {
	s32  left;        /* index of left  child (feature <  threshold) */
	s32  right;       /* index of right child (feature >= threshold) */
	s32  feature;     /* index into feature vector; DTREE_LEAF_SENTINEL for leaf */
	u64  threshold;   /* split value (quantized from sklearn float64) */
	u64  value;       /* predicted class (task index) — valid at leaves */
};

/* -------------------------------------------------------------------------
 * Inference
 * ---------------------------------------------------------------------- */

/*
 * dtree_infer - walk the tree and return the predicted task index.
 *
 * @nodes:    pointer to the flat node array
 * @n_nodes:  total number of nodes in the array
 * @features: feature vector (u64[])
 *
 * Returns the predicted task index, or 0 on any error (out-of-bounds,
 * malformed tree, infinite loop guard).
 */
static inline u64 dtree_infer(const struct dtree_node *nodes,
			       u32 n_nodes,
			       const u64 *features)
{
	u32 depth = 0;
	s32 cur   = 0;   /* start at root */

#define DTREE_MAX_DEPTH 64

	while (depth < DTREE_MAX_DEPTH) {
		if ((u32)cur >= n_nodes)
			return 0;

		const struct dtree_node *node = &nodes[cur];

		/* Leaf node */
		if (node->feature == DTREE_LEAF_SENTINEL)
			return node->value;

		/* Split: go left if feature < threshold, else right */
		u64 feat_val = features[(u32)node->feature];

		if (feat_val < node->threshold)
			cur = node->left;
		else
			cur = node->right;

		depth++;
	}

	/* Should never reach here with a well-formed tree */
	return 0;

#undef DTREE_MAX_DEPTH
}

/* -------------------------------------------------------------------------
 * Kernel hook adapter
 *
 * Matches cfs_mlp_infer_func_t: int (*)(u64 *features, u64 *out_index)
 * ---------------------------------------------------------------------- */

struct dtree_model {
	const struct dtree_node *nodes;
	u32                      n_nodes;
};

static inline int dtree_hook(const struct dtree_model *model,
			      u64 *features,
			      u64 *out_index)
{
	if (!model || !model->nodes || !features || !out_index)
		return -1;

	*out_index = dtree_infer(model->nodes, model->n_nodes, features);
	return 0;
}
