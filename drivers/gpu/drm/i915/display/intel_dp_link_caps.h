/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DP_LINK_CAPS_H__
#define __INTEL_DP_LINK_CAPS_H__

#include <linux/types.h>

struct intel_connector;
struct intel_dp;
struct intel_dp_link_caps;
struct intel_dp_link_config;

/**
 * enum intel_dp_link_caps_config_order_key - key used to order configurations
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW:
 *   Order configurations by bandwidth, then by link rate.
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_NUM:
 *   Number of ordering keys.
 *
 * Selects how a caller wants the configuration table to be ordered,
 * together with an &enum intel_dp_link_caps_config_order_direction, for
 * iteration queries.
 *
 * See also:
 *  - &struct intel_dp_link_caps_config_order
 */
enum intel_dp_link_caps_config_order_key {
	INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW,

	INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_NUM
};

/**
 * enum intel_dp_link_caps_config_order_direction - iteration direction
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC:
 *   Iterate in ascending order according to the selected ordering key.
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_NUM:
 *   Number of ordering directions.
 *
 * Selects the direction associated with an
 * &enum intel_dp_link_caps_config_order_key for iteration queries.
 *
 * See also:
 *  - &struct intel_dp_link_caps_config_order
 */
enum intel_dp_link_caps_config_order_direction {
	INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC,

	INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_NUM
};

/**
 * struct intel_dp_link_caps_config_order - configuration ordering
 * @key:
 *   Key used to order configurations, see
 *   &enum intel_dp_link_caps_config_order_key.
 * @dir:
 *   Direction of the selected ordering, see
 *   &enum intel_dp_link_caps_config_order_direction.
 *
 * Describes an iteration order for link configurations.
 *
 * See also:
 *  - intel_dp_link_caps_get_config_by_pos()
 */
struct intel_dp_link_caps_config_order {
	enum intel_dp_link_caps_config_order_key key;
	enum intel_dp_link_caps_config_order_direction dir;
};

/**
 * enum intel_dp_link_caps_config_match_type - configuration match semantics
 * @INTEL_DP_LINK_CAPS_CONFIG_MATCH_EXACT:
 *   Require an exact nominal link rate match and an
 *   exact lane count match.
 * @INTEL_DP_LINK_CAPS_CONFIG_MATCH_FUZZY_RATE:
 *   Require an exact lane count match, but allow the
 *   requested link rate to match approximately to a
 *   supported nominal link rate.
 *
 * Selects how
 * intel_dp_link_caps_find_allowed_config_pos() matches
 * the requested &struct intel_dp_link_config against the currently
 * allowed configurations.
 */
enum intel_dp_link_caps_config_match_type {
	INTEL_DP_LINK_CAPS_CONFIG_MATCH_EXACT,
	INTEL_DP_LINK_CAPS_CONFIG_MATCH_FUZZY_RATE,
};

int intel_dp_link_caps_common_rate(struct intel_dp_link_caps *link_caps, int index);
int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps, int rate);
int intel_dp_link_caps_max_common_rate(struct intel_dp_link_caps *link_caps);
int intel_dp_link_caps_num_common_rates(struct intel_dp_link_caps *link_caps);
void intel_dp_link_caps_all_common_rates(struct intel_dp_link_caps *link_caps,
					 const int **rates, int *num_rates);

void intel_dp_link_caps_get_forced_params(struct intel_dp_link_caps *link_caps,
					  struct intel_dp_link_config *forced_params);
u32 intel_dp_link_caps_get_allowed_config_mask(struct intel_dp_link_caps *link_caps);

int intel_dp_link_config_index(struct intel_dp_link_caps *link_caps,
			       int link_rate, int lane_count);
bool
intel_dp_link_caps_get_config_by_pos(struct intel_dp_link_caps *link_caps,
				     struct intel_dp_link_caps_config_order config_order,
				     int iter_pos,
				     struct intel_dp_link_config *config, int *config_idx);
void intel_dp_link_config_get(struct intel_dp_link_caps *link_caps,
			      int idx, int *link_rate, int *lane_count);
int intel_dp_link_caps_find_allowed_config_pos(struct intel_dp_link_caps *link_caps,
					       struct intel_dp_link_caps_config_order order,
					       enum intel_dp_link_caps_config_match_type match_type,
					       const struct intel_dp_link_config *config);

void intel_dp_link_caps_get_max_limits(struct intel_dp_link_caps *link_caps,
				       struct intel_dp_link_config *max_link_limits);
bool intel_dp_link_caps_set_max_limits(struct intel_dp_link_caps *link_caps,
				       const struct intel_dp_link_config *max_link_limits);
void intel_dp_link_caps_reset_max_limits(struct intel_dp_link_caps *link_caps);

bool intel_dp_link_caps_update(struct intel_dp_link_caps *link_caps,
			       const int *rates, int num_rates, int max_lane_count);
void intel_dp_link_caps_reset(struct intel_dp_link_caps *link_caps);

void intel_dp_link_caps_debugfs_add(struct intel_connector *connector);

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *intel_dp);
void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps);

#endif /* __INTEL_DP_LINK_CAPS_H__ */
