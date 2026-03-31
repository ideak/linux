// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/types.h>

#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_caps.h"

struct intel_dp_link_caps {
	struct intel_dp *dp;

	struct {
		int max_lane_count;
	} common_params;

	/* common rate,lane_count configs in bw order */
	int num_configs;
#define INTEL_DP_MAX_LANE_COUNT			4
#define INTEL_DP_MAX_SUPPORTED_LANE_CONFIGS	(ilog2(INTEL_DP_MAX_LANE_COUNT) + 1)
#define INTEL_DP_LANE_COUNT_EXP_BITS		order_base_2(INTEL_DP_MAX_SUPPORTED_LANE_CONFIGS)
#define INTEL_DP_LINK_RATE_IDX_BITS		(BITS_PER_TYPE(u8) - INTEL_DP_LANE_COUNT_EXP_BITS)
#define INTEL_DP_MAX_LINK_CONFIGS		(DP_MAX_SUPPORTED_RATES * \
						 INTEL_DP_MAX_SUPPORTED_LANE_CONFIGS)
	struct intel_dp_link_config_entry {
		u8 link_rate_idx:INTEL_DP_LINK_RATE_IDX_BITS;
		u8 lane_count_exp:INTEL_DP_LANE_COUNT_EXP_BITS;
	} configs[INTEL_DP_MAX_LINK_CONFIGS];
};

/* Get length of common rates array potentially limited by max_rate. */
int intel_dp_link_caps_common_len_rate_limit(struct intel_dp_link_caps *link_caps,
					     int max_rate)
{
	struct intel_dp *intel_dp = link_caps->dp;

	return intel_dp_rate_limit_len(intel_dp->common_rates,
				       intel_dp->num_common_rates, max_rate);
}

int intel_dp_link_caps_common_rate(struct intel_dp_link_caps *link_caps, int index)
{
	struct intel_dp *intel_dp = link_caps->dp;
	struct intel_display *display = to_intel_display(intel_dp);

	if (drm_WARN_ON(display->drm,
			index < 0 || index >= intel_dp->num_common_rates))
		return 162000;

	return intel_dp->common_rates[index];
}

int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps, int rate)
{
	struct intel_dp *intel_dp = link_caps->dp;

	return intel_dp_rate_index(intel_dp->common_rates,
				   intel_dp->num_common_rates,
				   rate);
}

/* Theoretical max between source and sink */
int intel_dp_link_caps_max_common_rate(struct intel_dp_link_caps *link_caps)
{
	struct intel_dp *intel_dp = link_caps->dp;

	return intel_dp_link_caps_common_rate(link_caps, intel_dp->num_common_rates - 1);
}

void intel_dp_link_caps_all_common_rates(struct intel_dp_link_caps *link_caps,
					 const int **rates, int *num_rates)
{
	struct intel_dp *intel_dp = link_caps->dp;

	*rates = intel_dp->common_rates;
	*num_rates = intel_dp->num_common_rates;
}

int intel_dp_link_caps_max_common_lane_count(struct intel_dp_link_caps *link_caps)
{
	return link_caps->common_params.max_lane_count;
}

static int intel_dp_link_config_rate(struct intel_dp_link_caps *link_caps,
				     const struct intel_dp_link_config_entry *lc)
{
	return intel_dp_link_caps_common_rate(link_caps, lc->link_rate_idx);
}

static int intel_dp_link_config_lane_count(const struct intel_dp_link_config_entry *lc)
{
	return 1 << lc->lane_count_exp;
}

static int intel_dp_link_config_bw(struct intel_dp_link_caps *link_caps,
				   const struct intel_dp_link_config_entry *lc)
{
	return drm_dp_max_dprx_data_rate(intel_dp_link_config_rate(link_caps, lc),
					 intel_dp_link_config_lane_count(lc));
}

static int link_config_cmp_by_bw(const void *a, const void *b, const void *p)
{
	struct intel_dp_link_caps *link_caps = (struct intel_dp_link_caps *)p;	/* remove const */
	const struct intel_dp_link_config_entry *lc_a = a;
	const struct intel_dp_link_config_entry *lc_b = b;
	int bw_a = intel_dp_link_config_bw(link_caps, lc_a);
	int bw_b = intel_dp_link_config_bw(link_caps, lc_b);

	if (bw_a != bw_b)
		return bw_a - bw_b;

	return intel_dp_link_config_rate(link_caps, lc_a) -
	       intel_dp_link_config_rate(link_caps, lc_b);
}

void intel_dp_link_caps_update(struct intel_dp_link_caps *link_caps,
			       int max_lane_count)
{
	struct intel_dp *intel_dp = link_caps->dp;
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_dp_link_config_entry *lc;
	int num_common_lane_configs;
	int i;
	int j;

	if (drm_WARN_ON(display->drm, !is_power_of_2(max_lane_count)))
		return;

	num_common_lane_configs = ilog2(max_lane_count) + 1;

	if (drm_WARN_ON(display->drm, intel_dp->num_common_rates * num_common_lane_configs >
				    ARRAY_SIZE(link_caps->configs)))
		return;

	link_caps->common_params.max_lane_count = max_lane_count;

	link_caps->num_configs = intel_dp->num_common_rates * num_common_lane_configs;

	lc = &link_caps->configs[0];
	for (i = 0; i < intel_dp->num_common_rates; i++) {
		for (j = 0; j < num_common_lane_configs; j++) {
			lc->lane_count_exp = j;
			lc->link_rate_idx = i;

			lc++;
		}
	}

	sort_r(link_caps->configs, link_caps->num_configs,
	       sizeof(link_caps->configs[0]),
	       link_config_cmp_by_bw, NULL,
	       link_caps);
}

void intel_dp_link_config_get(struct intel_dp_link_caps *link_caps,
			      int idx, int *link_rate, int *lane_count)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	const struct intel_dp_link_config_entry *lc;

	if (drm_WARN_ON(display->drm, idx < 0 || idx >= link_caps->num_configs))
		idx = 0;

	lc = &link_caps->configs[idx];

	*link_rate = intel_dp_link_config_rate(link_caps, lc);
	*lane_count = intel_dp_link_config_lane_count(lc);
}

int intel_dp_link_config_index(struct intel_dp_link_caps *link_caps,
			       int link_rate, int lane_count)
{
	struct intel_dp *intel_dp = link_caps->dp;
	int link_rate_idx = intel_dp_rate_index(intel_dp->common_rates,
						intel_dp->num_common_rates,
						link_rate);
	int lane_count_exp = ilog2(lane_count);
	int i;

	for (i = 0; i < link_caps->num_configs; i++) {
		const struct intel_dp_link_config_entry *lc = &link_caps->configs[i];

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
