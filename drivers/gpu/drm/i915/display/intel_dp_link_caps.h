/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DP_LINK_CAPS_H__
#define __INTEL_DP_LINK_CAPS_H__

struct intel_dp;
struct intel_dp_link_caps;

int intel_dp_link_caps_common_len_rate_limit(struct intel_dp_link_caps *link_caps,
					     int max_rate);
int intel_dp_link_caps_max_common_rate(struct intel_dp_link_caps *link_caps);
int intel_dp_link_caps_common_rate(struct intel_dp_link_caps *link_caps, int index);
int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps, int rate);
void intel_dp_link_caps_all_common_rates(struct intel_dp_link_caps *link_caps,
					 const int **rates, int *num_rates);

int intel_dp_link_caps_max_common_lane_count(struct intel_dp_link_caps *link_caps);

int intel_dp_link_config_index(struct intel_dp_link_caps *link_caps,
			       int link_rate, int lane_count);
void intel_dp_link_config_get(struct intel_dp_link_caps *link_caps,
			      int idx, int *link_rate, int *lane_count);

void intel_dp_link_caps_update(struct intel_dp_link_caps *link_caps,
			       int max_lane_count);

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *dp);
void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps);

#endif /* __INTEL_DP_LINK_CAPS_H__ */
