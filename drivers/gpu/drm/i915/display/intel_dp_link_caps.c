// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/sort.h>

#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_caps.h"

struct intel_dp_link_caps {
	struct intel_dp *dp;
};

static int intel_dp_link_config_rate(struct intel_dp *intel_dp,
				     const struct intel_dp_link_config *lc)
{
	return intel_dp_common_rate(intel_dp, lc->link_rate_idx);
}

static int intel_dp_link_config_lane_count(const struct intel_dp_link_config *lc)
{
	return 1 << lc->lane_count_exp;
}

static int intel_dp_link_config_bw(struct intel_dp *intel_dp,
				   const struct intel_dp_link_config *lc)
{
	return drm_dp_max_dprx_data_rate(intel_dp_link_config_rate(intel_dp, lc),
					 intel_dp_link_config_lane_count(lc));
}

static int link_config_cmp_by_bw(const void *a, const void *b, const void *p)
{
	struct intel_dp *intel_dp = (struct intel_dp *)p;	/* remove const */
	const struct intel_dp_link_config *lc_a = a;
	const struct intel_dp_link_config *lc_b = b;
	int bw_a = intel_dp_link_config_bw(intel_dp, lc_a);
	int bw_b = intel_dp_link_config_bw(intel_dp, lc_b);

	if (bw_a != bw_b)
		return bw_a - bw_b;

	return intel_dp_link_config_rate(intel_dp, lc_a) -
	       intel_dp_link_config_rate(intel_dp, lc_b);
}

void intel_dp_link_config_init(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_dp_link_config *lc;
	int num_common_lane_configs;
	int i;
	int j;

	if (drm_WARN_ON(display->drm, !is_power_of_2(intel_dp_max_common_lane_count(intel_dp))))
		return;

	num_common_lane_configs = ilog2(intel_dp_max_common_lane_count(intel_dp)) + 1;

	if (drm_WARN_ON(display->drm, intel_dp->num_common_rates * num_common_lane_configs >
				    ARRAY_SIZE(intel_dp->link.configs)))
		return;

	intel_dp->link.num_configs = intel_dp->num_common_rates * num_common_lane_configs;

	lc = &intel_dp->link.configs[0];
	for (i = 0; i < intel_dp->num_common_rates; i++) {
		for (j = 0; j < num_common_lane_configs; j++) {
			lc->lane_count_exp = j;
			lc->link_rate_idx = i;

			lc++;
		}
	}

	sort_r(intel_dp->link.configs, intel_dp->link.num_configs,
	       sizeof(intel_dp->link.configs[0]),
	       link_config_cmp_by_bw, NULL,
	       intel_dp);
}

void intel_dp_link_config_get(struct intel_dp *intel_dp, int idx, int *link_rate, int *lane_count)
{
	struct intel_display *display = to_intel_display(intel_dp);
	const struct intel_dp_link_config *lc;

	if (drm_WARN_ON(display->drm, idx < 0 || idx >= intel_dp->link.num_configs))
		idx = 0;

	lc = &intel_dp->link.configs[idx];

	*link_rate = intel_dp_link_config_rate(intel_dp, lc);
	*lane_count = intel_dp_link_config_lane_count(lc);
}

int intel_dp_link_config_index(struct intel_dp *intel_dp, int link_rate, int lane_count)
{
	int link_rate_idx = intel_dp_rate_index(intel_dp->common_rates, intel_dp->num_common_rates,
						link_rate);
	int lane_count_exp = ilog2(lane_count);
	int i;

	for (i = 0; i < intel_dp->link.num_configs; i++) {
		const struct intel_dp_link_config *lc = &intel_dp->link.configs[i];

		if (lc->lane_count_exp == lane_count_exp &&
		    lc->link_rate_idx == link_rate_idx)
			return i;
	}

	return -1;
}

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *dp)
{
	struct intel_dp_link_caps *link_caps;

	link_caps = kzalloc_obj(*link_caps);
	if (!link_caps)
		return NULL;

	link_caps->dp = dp;

	return link_caps;
}

void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps)
{
	kfree(link_caps);
}
