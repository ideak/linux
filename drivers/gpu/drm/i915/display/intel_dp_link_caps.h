/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DP_LINK_CAPS_H__
#define __INTEL_DP_LINK_CAPS_H__

struct intel_dp;

int intel_dp_link_config_index(struct intel_dp *intel_dp, int link_rate, int lane_count);
void intel_dp_link_config_get(struct intel_dp *intel_dp, int idx, int *link_rate, int *lane_count);

void intel_dp_link_config_init(struct intel_dp *intel_dp);

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *dp);
void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps);

#endif /* __INTEL_DP_LINK_CAPS_H__ */
