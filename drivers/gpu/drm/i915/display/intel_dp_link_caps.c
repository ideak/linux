// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/slab.h>

#include "intel_dp_link_caps.h"

struct intel_dp_link_caps {
	struct intel_dp *dp;
};

/**
 * intel_dp_link_caps_init - allocate and initialize link caps state
 * @intel_dp: DP encoder state
 *
 * Allocate and initialize the link capabilities state for @intel_dp and
 * the connectors attached to it.
 *
 * Return:
 * - Pointer to the newly allocated link capabilities state.
 * - %NULL if allocation fails.
 */
struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *intel_dp)
{
	struct intel_dp_link_caps *link_caps;

	link_caps = kzalloc_obj(*link_caps);
	if (!link_caps)
		return NULL;

	link_caps->dp = intel_dp;

	return link_caps;
}

/**
 * intel_dp_link_caps_cleanup - free link caps state
 * @link_caps: link capabilities state to free
 *
 * Free @link_caps.
 */
void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps)
{
	kfree(link_caps);
}
