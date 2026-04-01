/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DP_LINK_CAPS_H__
#define __INTEL_DP_LINK_CAPS_H__

#include <linux/bitops.h>
#include <linux/types.h>

struct intel_connector;
struct intel_dp;
struct intel_dp_link_caps;
struct intel_dp_link_config;

/**
 * enum intel_dp_link_caps_config_order_key - key used to order configurations
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW:
 *    Order configurations by bandwidth. Ties are resolved by
 *    increasing link rate.
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_RATE_LANE:
 *    Order configurations by nominal link rate first and by
 *    lane count second.
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_NUM:
 *    Number of ordering keys.
 *
 * Selects how a caller wants the configuration table to be
 * ordered for "maximum configuration" queries or, together
 * with an &enum intel_dp_link_caps_config_order_direction,
 * for iteration queries.
 *
 * See also:
 * - &struct intel_dp_link_caps_config_order
 * - intel_dp_link_caps_max_config()
 * - intel_dp_link_caps_max_config_idx()
 */
enum intel_dp_link_caps_config_order_key {
	INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW,
	INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_RATE_LANE,

	INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_NUM
};

/**
 * enum intel_dp_link_caps_config_order_direction - iteration direction
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC:
 *    Iterate in ascending order according to the selected ordering key.
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_DESC:
 *    Iterate in descending order according to the selected ordering key.
 * @INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_NUM:
 *    Number of ordering directions.
 *
 * Selects the direction associated with an
 * &enum intel_dp_link_caps_config_order_key for iteration queries.
 *
 * See also:
 * - &struct intel_dp_link_caps_config_order
 */
enum intel_dp_link_caps_config_order_direction {
	INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC,
	INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_DESC,

	INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_NUM
};

/**
 * struct intel_dp_link_caps_config_order - configuration ordering
 * @key:
 *	Key used to order configurations, see
 *	&enum intel_dp_link_caps_config_order_key.
 * @dir:
 *	Direction of the selected ordering, see
 *	&enum intel_dp_link_caps_config_order_direction.
 *
 * Describes an iteration order for link configurations.
 *
 * See also:
 * - intel_dp_link_caps_config_order_for_connector()
 * - intel_dp_link_caps_config_iter_at()
 * - for_each_dp_link_config_idx()
 * - for_each_dp_link_config()
 */
struct intel_dp_link_caps_config_order {
	enum intel_dp_link_caps_config_order_key key;
	enum intel_dp_link_caps_config_order_direction dir;
};

/**
 * enum intel_dp_link_caps_config_match_type - configuration match semantics
 * @INTEL_DP_LINK_CAPS_CONFIG_MATCH_EXACT:
 *    Require an exact nominal link-rate match and an
 *    exact lane-count match.
 * @INTEL_DP_LINK_CAPS_CONFIG_MATCH_FUZZY_RATE:
 *    Require an exact lane-count match, but allow the
 *    requested link rate to match approximately to a
 *    supported nominal link rate.
 *
 * Selects how intel_dp_link_caps_find_allowed_config() matches
 * the requested &struct intel_dp_link_config against the currently
 * allowed configurations.
 */
enum intel_dp_link_caps_config_match_type {
	INTEL_DP_LINK_CAPS_CONFIG_MATCH_EXACT,
	INTEL_DP_LINK_CAPS_CONFIG_MATCH_FUZZY_RATE,
};

/**
 * for_each_dp_link_config_idx - iterate selected configurations and indices
 * @__link_caps: &struct intel_dp_link_caps being queried
 * @__config_order: &struct intel_dp_link_caps_config_order describing the
 *    iteration order
 * @__config_mask: mask of configuration indices to visit
 * @__config: pointer to &struct intel_dp_link_config filled for each match
 * @__config_idx: optional pointer to the configuration index
 *
 * Iterate the configurations selected by @__config_mask in the order described
 * by @__config_order.
 *
 * The configuration mask uses the canonical configuration indexing shared by
 * the whole API.
 *
 * This iterator calls intel_dp_link_caps_config_iter_at() internally, so the
 * same locking rules apply: the caller must serialize iteration against
 * concurrent updates and concurrent queries.
 */
#define for_each_dp_link_config_idx(__link_caps, __config_order, __config_mask, \
				    __config, __config_idx) \
	for (int __iter_pos = 0, \
	         __iter_config_idx, \
		 *__config_idx_p = (__config_idx); /* Avoid always-true address warning */ \
	     intel_dp_link_caps_config_iter_at((__link_caps), (__config_order), (__iter_pos), \
					       (__config), (__config_idx_p) ? : &(__iter_config_idx)); \
	     (__iter_pos)++) \
	     for_each_if ((__config_mask) & BIT((__config_idx_p) ? *(__config_idx_p) : __iter_config_idx))

/**
 * for_each_dp_link_config - iterate selected configurations
 * @__link_caps: &struct intel_dp_link_caps being queried
 * @__config_order: struct intel_dp_link_caps_config_order describing the
 *    iteration order
 * @__config_mask: canonical mask of configurations to visit
 * @__config: pointer to &struct intel_dp_link_config filled for each match
 *
 * Iterate the configurations selected by @__config_mask in the order described
 * by @__config_order, without returning the canonical configuration index.
 *
 * See also:
 * - for_each_dp_link_config_idx()
 */
#define for_each_dp_link_config(__link_caps, __config_order, __config_mask, __config) \
	for_each_dp_link_config_idx((__link_caps), (__config_order), (__config_mask), \
				    (__config), NULL)

struct intel_dp_link_caps_config_order
intel_dp_link_caps_config_order_for_connector(struct intel_connector *connector);

int intel_dp_link_caps_common_rate_at(struct intel_dp_link_caps *link_caps,
				      int idx);
int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps,
				       int rate);
int intel_dp_link_caps_max_common_rate(struct intel_dp_link_caps *link_caps);
void intel_dp_link_caps_all_common_rates(struct intel_dp_link_caps *link_caps,
					 const int **rates, int *num_rates);

bool
intel_dp_link_caps_config_iter_at(struct intel_dp_link_caps *link_caps,
				  struct intel_dp_link_caps_config_order config_order,
				  int iter_pos,
				  struct intel_dp_link_config *config, int *config_idx);

void intel_dp_link_caps_forced_params(struct intel_dp_link_caps *link_caps,
				      struct intel_dp_link_config *forced_params);
u32 intel_dp_link_caps_allowed_config_mask(struct intel_dp_link_caps *link_caps);

bool intel_dp_link_caps_config_at(struct intel_dp_link_caps *link_caps,
				  int config_idx,
				  struct intel_dp_link_config *config);
int intel_dp_link_caps_find_allowed_config(struct intel_dp_link_caps *link_caps,
					   const struct intel_dp_link_config *link_config,
					   enum intel_dp_link_caps_config_match_type match_type);

void intel_dp_link_caps_max_config(struct intel_dp_link_caps *link_caps,
				   enum intel_dp_link_caps_config_order_key order_key,
				   u32 config_mask, struct intel_dp_link_config *config);
int intel_dp_link_caps_max_config_idx(struct intel_dp_link_caps *link_caps,
				      enum intel_dp_link_caps_config_order_key order_key,
				      u32 config_mask);
void intel_dp_link_caps_max_bw_config(struct intel_dp_link_caps *link_caps,
				      struct intel_dp_link_config *config);

bool intel_dp_link_caps_disable_config(struct intel_dp_link_caps *link_caps, int idx);

bool intel_dp_link_caps_set_max_limits(struct intel_dp_link_caps *link_caps,
				       const struct intel_dp_link_config *link_config);
void intel_dp_link_caps_get_max_limits(struct intel_dp_link_caps *link_caps,
				       struct intel_dp_link_config *max_link_limits);
void intel_dp_link_caps_reset_max_limits(struct intel_dp_link_caps *link_caps);

void intel_dp_link_caps_update(struct intel_dp_link_caps *link_caps,
			       const int *rates, int num_rates,
			       int max_lane_count);
void intel_dp_link_caps_reset(struct intel_dp_link_caps *link_caps);

void intel_dp_link_caps_debugfs_add(struct intel_connector *connector);

struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *dp);
void intel_dp_link_caps_cleanup(struct intel_dp_link_caps *link_caps);

#endif /* __INTEL_DP_LINK_CAPS_H__ */
