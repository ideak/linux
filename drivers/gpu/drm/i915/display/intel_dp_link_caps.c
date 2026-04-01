// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/types.h>

#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_caps.h"
#include "intel_dp_link_training.h"

/**
 * DOC: Intel DP link caps API
 *
 * The Intel DP link caps API tracks the supported and allowed DP link
 * configurations for a DP encoder and its attached connectors, with helpers
 * to iterate, filter, disable, and constrain them.
 *
 * Locking:
 * Unless stated otherwise, the caller must serialize all accesses to this API
 * against concurrent updates and queries. The only exceptions are
 * intel_dp_link_caps_get_max_limits() and intel_dp_link_caps_max_bw_config(),
 * for which a lockless lookup is allowed. However, those lockless lookups may
 * observe a torn &struct intel_dp_link_config tuple, i.e. a rate from one
 * state and a lane count from another.
 *
 * Configuration indexing:
 * Configuration indices and configuration masks used by this API always refer
 * to the same configurations across the whole API.
 *
 * The indices are stable only between calls to intel_dp_link_caps_update(),
 * which rebuilds the configuration table and may therefore change the index
 * of any configuration, including configurations that already existed before
 * the update.
 *
 * This is different from an iteration position. The @iter_pos argument used
 * by intel_dp_link_caps_config_iter_at() is only a position within the
 * iteration order selected by &struct intel_dp_link_caps_config_order and is
 * not a configuration index.
 *
 * For now the ascending BW order iteration position happens to match the
 * configuration index, but callers must not rely on that.
 */

struct intel_dp_link_caps {
	struct intel_dp *dp;

	struct intel_dp_link_caps_config_table {
		int num_rates;
		int rates[DP_MAX_SUPPORTED_RATES];
		int max_lane_count;

		/* common rate,lane_count configs in bw order */
		int num_configs;
#define INTEL_DP_MAX_LANE_COUNT			4
#define INTEL_DP_MAX_SUPPORTED_LANE_CONFIGS	(ilog2(INTEL_DP_MAX_LANE_COUNT) + 1)
#define INTEL_DP_LANE_COUNT_EXP_BITS		order_base_2(INTEL_DP_MAX_SUPPORTED_LANE_CONFIGS)
#define INTEL_DP_LINK_RATE_IDX_BITS		(BITS_PER_TYPE(u8) - INTEL_DP_LANE_COUNT_EXP_BITS)
#define INTEL_DP_MAX_LINK_CONFIGS		(DP_MAX_SUPPORTED_RATES * \
						 INTEL_DP_MAX_SUPPORTED_LANE_CONFIGS)
		struct intel_dp_link_config_entry {
			/* index into common_params.rates[] */
			u8 link_rate_idx:INTEL_DP_LINK_RATE_IDX_BITS;
			u8 lane_count_exp:INTEL_DP_LANE_COUNT_EXP_BITS;
		} configs[INTEL_DP_MAX_LINK_CONFIGS];
		/* indices of configs[] in rate,lane_count order */
		u8 rate_lane_order_idx[INTEL_DP_MAX_LINK_CONFIGS];

		/*
		 * Mask of configs disabled for the current sink connection.
		 *
		 * Each bit directly indexes configs[], which is stored in
		 * ascending BW order. Helper-derived config masks, such as
		 * all, forced, and allowed masks, use the same indexing, and
		 * config indices returned by the API refer to the same
		 * configs[] entries.
		 *
		 * Users can only disable configs by setting bits in this mask.
		 * Bits are cleared only by the link caps code, e.g. on sink
		 * disconnect, when forcing a link rate or lane count, or while
		 * recovering from invalid/error cases.
		 */
		u32 disabled_config_mask;
	} config_table;

	/*
	 * Forced parameters requested via debugfs. Remains set across sink
	 * disconnects.
	 */
	struct intel_dp_link_config forced_params;

	/* Cached parameters of the allowed configuration with the highest BW. */
	struct intel_dp_link_config max_bw_config;

	/*
	 * Cached upper bounds of the allowed link configurations.
	 *
	 * The max rate and max lane count in max_limits may come from different
	 * allowed configurations, IOW the max rate and max lane count may not
	 * form an actual allowed configuration.
	 *
	 * Currently max_limits also constrains the allowed configs, so it may
	 * need to increase while the sink remains connected, e.g. after link
	 * training fallback selects a config above the previous max_limits.
	 *
	 * These limits are reset on sink disconnect and when forcing a link
	 * rate or lane count, restoring them to the maximum parameters of the
	 * enabled configurations, constrained only by any forced parameters.
	 *
	 * TODO: Make max_limits just reflect the maximum of the allowed
	 * configs at all times. Then it will no longer constrain them and
	 * will never need to increase.
	 */
	struct intel_dp_link_config max_limits;
};
static_assert(BITS_PER_TYPE(u32) >=
	      ARRAY_SIZE(((struct intel_dp_link_caps *)NULL)->config_table.configs));

static struct intel_dp_link_caps_config_order bw_asc_config_order(void)
{
	struct intel_dp_link_caps_config_order order = {
		.key = INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW,
		.dir = INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC
	};

	return order;
}

static struct intel_dp_link_caps *connector_to_dp_link_caps(struct intel_connector *connector)
{
	return intel_attached_dp(connector)->link.caps;
}

static enum intel_dp_link_caps_config_order_key
order_key_for_connector(struct intel_connector *connector)
{
	if (connector->mst.dp)
		return INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW;
	else
		return INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_RATE_LANE;
}

static enum intel_dp_link_caps_config_order_direction
order_dir_for_connector(struct intel_connector *connector)
{
	struct intel_dp *intel_dp = intel_attached_dp(connector);

	if (connector->mst.dp || intel_dp->use_max_params)
		return INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_DESC;
	else
		return INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC;
}

/**
 * intel_dp_link_caps_config_order_for_connector - get config iteration order
 * @connector: connector to get the iteration order for
 *
 * Return the configuration ordering to use for @connector.
 *
 * The returned order is suitable for the for_each_dp_link_config()
 * iterators and related APIs that take a
 * &struct intel_dp_link_caps_config_order.
 *
 * The caller must serialize this call against changes to @connector->mst.dp
 * and intel_attached_dp(@connector)->use_max_params. In practice these are
 * updated from the connector's detect path, so callers should hold
 * &drm_mode_config.connection_mutex.
 *
 * Return:
 * - Configuration ordering for @connector.
 */
struct intel_dp_link_caps_config_order
intel_dp_link_caps_config_order_for_connector(struct intel_connector *connector)
{
	struct intel_dp_link_caps_config_order order = {
		.key = order_key_for_connector(connector),
		.dir = order_dir_for_connector(connector)
	};

	return order;
}

static int lookup_rate(const struct intel_dp_link_caps_config_table *table, int idx)
{
	if (WARN_ON(idx < 0 || idx >= table->num_rates))
		return 162000;

	return table->rates[idx];
}

/**
 * intel_dp_link_caps_common_rate_at - get common link rate at a given index
 * @link_caps: link capabilities state
 * @idx: index into the common rate list
 *
 * Return the common link rate identified by @idx currently supported by
 * @link_caps.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - Common link rate at @idx.
 * - 162000 if @idx is out of range.
 */
int intel_dp_link_caps_common_rate_at(struct intel_dp_link_caps *link_caps, int idx)
{
	return lookup_rate(&link_caps->config_table, idx);
}

/**
 * intel_dp_link_caps_common_rate_idx - get index of a common link rate
 * @link_caps: link capabilities state
 * @rate: common link rate to look up
 *
 * Look up @rate in the common rate list currently supported by
 * @link_caps.
 *
 * The returned value is an index into the common rate list returned by
 * intel_dp_link_caps_all_common_rates() and accepted by
 * intel_dp_link_caps_common_rate_at().
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - Index of @rate in the current common rate list.
 * - %-1 if @rate is not present.
 */
int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps, int rate)
{
	return intel_dp_rate_index(link_caps->config_table.rates,
				   link_caps->config_table.num_rates,
				   rate);
}

/**
 * intel_dp_link_caps_max_common_rate - get the maximum common link rate
 * @link_caps: link capabilities state
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - Maximum common link rate currently supported by @link_caps.
 */
int intel_dp_link_caps_max_common_rate(struct intel_dp_link_caps *link_caps)
{
	return intel_dp_link_caps_common_rate_at(link_caps, link_caps->config_table.num_rates - 1);
}

static int intel_dp_link_caps_max_common_lane_count(struct intel_dp_link_caps *link_caps)
{
	return link_caps->config_table.max_lane_count;
}

/**
 * intel_dp_link_caps_all_common_rates - get all common link rates
 * @link_caps: link capabilities state
 * @rates: returned pointer to the common rate array
 * @num_rates: returned number of entries in @rates
 *
 * Return all common link rates through @rates and @num_rates that
 * are currently supported by @link_caps. The returned array is
 * owned by @link_caps.
 *
 * The caller must serialize this call, and any dereference of the
 * returned array, against concurrent updates to @link_caps.
 */
void intel_dp_link_caps_all_common_rates(struct intel_dp_link_caps *link_caps,
					 const int **rates, int *num_rates)
{
	*rates = link_caps->config_table.rates;
	*num_rates = link_caps->config_table.num_rates;
}

static int link_config_entry_rate(const struct intel_dp_link_caps_config_table *table,
				  const struct intel_dp_link_config_entry *config_entry)
{
	return lookup_rate(table, config_entry->link_rate_idx);
}

static int link_config_entry_lane_count(const struct intel_dp_link_config_entry *config_entry)
{
	return 1 << config_entry->lane_count_exp;
}

static void
to_intel_dp_link_config(const struct intel_dp_link_caps_config_table *table,
			const struct intel_dp_link_config_entry *config_entry,
			struct intel_dp_link_config *config)
{
	config->rate = link_config_entry_rate(table, config_entry);
	config->lane_count = link_config_entry_lane_count(config_entry);
}

/**
 * intel_dp_link_caps_config_iter_at - get config at a given iterator position
 * @link_caps: link capability state
 * @config_order: iteration order requested by the caller
 * @iter_pos: zero-based position in the requested iteration order
 * @config: returned link configuration
 * @config_idx: returned config index
 *
 * Look up the link config at iterator position @iter_pos in the order
 * described by @config_order.
 *
 * Note that @iter_pos is an iteration position and is not related to the
 * returned @config_idx. The returned @config_idx always, regardless of
 * @config_order, uses the canonical configuration index/mask scheme shared by
 * the link-caps API.
 *
 * For now @iter_pos and @config_idx happen to match for ascending BW order,
 * but callers must not rely on that.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - %true if @iter_pos is valid, storing the configuration in @config and
 *   its index in @config_idx.
 * - %false if @iter_pos is out of range, storing
 *   %INTEL_DP_LINK_CONFIG_NULL in @config and -1 in @config_idx.
 */
bool intel_dp_link_caps_config_iter_at(struct intel_dp_link_caps *link_caps,
				       struct intel_dp_link_caps_config_order config_order,
				       int iter_pos,
				       struct intel_dp_link_config *config, int *config_idx)
{
	*config_idx = iter_pos;

	if (!in_range(*config_idx, 0, link_caps->config_table.num_configs)) {
		*config = INTEL_DP_LINK_CONFIG_NULL;
		*config_idx = -1;

		return false;
	}

	if (config_order.dir == INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_DESC)
		*config_idx = link_caps->config_table.num_configs - 1 - *config_idx;

	if (config_order.key == INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_RATE_LANE)
		*config_idx = link_caps->config_table.rate_lane_order_idx[*config_idx];

	to_intel_dp_link_config(&link_caps->config_table,
				&link_caps->config_table.configs[*config_idx],
				config);

	return true;
}

static int align_forced_lane_count(struct intel_dp_link_caps *link_caps,
				   int lane_count)
{
	if (lane_count == 0)
		return 0;

	return min(lane_count, link_caps->config_table.max_lane_count);
}

static int get_aligned_forced_lane_count(struct intel_dp_link_caps *link_caps)
{
	return align_forced_lane_count(link_caps,
				       link_caps->forced_params.lane_count);
}

static bool set_forced_lane_count(struct intel_dp_link_caps *link_caps,
				  int forced_lane_count)
{
	if (align_forced_lane_count(link_caps, forced_lane_count) >
	    link_caps->max_limits.lane_count)
		return false;

	link_caps->forced_params.lane_count = forced_lane_count;

	return true;
}

static int align_forced_link_rate(struct intel_dp_link_caps *link_caps,
				  int forced_link_rate)
{
	int len;

	if (forced_link_rate == 0)
		return 0;

	len = intel_dp_rate_limit_len(link_caps->config_table.rates,
				      link_caps->config_table.num_rates,
				      forced_link_rate);

	if (len == 0)
		return intel_dp_link_caps_common_rate_at(link_caps, 0);
	else
		return intel_dp_link_caps_common_rate_at(link_caps, len - 1);
}

static int get_aligned_forced_rate(struct intel_dp_link_caps *link_caps)
{
	return align_forced_link_rate(link_caps,
				      link_caps->forced_params.rate);
}

static bool set_forced_link_rate(struct intel_dp_link_caps *link_caps,
				 int forced_link_rate)
{
	if (align_forced_link_rate(link_caps, forced_link_rate) >
	    link_caps->max_limits.rate)
		return false;

	link_caps->forced_params.rate = forced_link_rate;

	return true;
}

/**
 * intel_dp_link_caps_forced_params - get the effective forced link parameters
 * @link_caps: link capabilities state
 * @forced_params: returned forced link parameters
 *
 * Return the user-requested forced link parameters in @forced_params, bounded
 * independently by the supported link rates and maximum lane count. Together
 * they may therefore not form a supported link configuration. A parameter is
 * set to 0 in @forced_params if that parameter is not forced.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 */
void intel_dp_link_caps_forced_params(struct intel_dp_link_caps *link_caps,
				      struct intel_dp_link_config *forced_params)
{
	forced_params->rate = get_aligned_forced_rate(link_caps);
	forced_params->lane_count = get_aligned_forced_lane_count(link_caps);
}

static u32 intel_dp_link_caps_all_config_mask(struct intel_dp_link_caps *link_caps)
{
	struct intel_display *display = to_intel_display(link_caps->dp);

	if (drm_WARN_ON(display->drm, link_caps->config_table.num_configs <= 0))
		return 0;

	return GENMASK(link_caps->config_table.num_configs - 1, 0);
}

/*
 * Build the mask of configs that remain usable after applying
 * link_caps->max_limits and any link_caps->forced_params
 * rate/lane-count constraints, without excluding explicitly
 * disabled configs.
 */
static u32 get_effective_config_mask(struct intel_dp_link_caps *link_caps)
{
	struct intel_dp_link_config forced_params;
	struct intel_dp_link_config config;
	u32 effective_config_mask = 0;
	int config_idx;

	intel_dp_link_caps_forced_params(link_caps, &forced_params);

	for_each_dp_link_config_idx(link_caps, bw_asc_config_order(),
				    intel_dp_link_caps_all_config_mask(link_caps),
				    &config, &config_idx) {
		if (forced_params.rate &&
		    config.rate != forced_params.rate)
			continue;

		if (forced_params.lane_count &&
		    config.lane_count != forced_params.lane_count)
			continue;

		if (config.rate > link_caps->max_limits.rate)
			continue;

		if (config.lane_count > link_caps->max_limits.lane_count)
			continue;

		effective_config_mask |= BIT(config_idx);
	}

	return effective_config_mask;
}

/**
 * intel_dp_link_caps_allowed_config_mask - get the currently allowed config mask
 * @link_caps: link capabilities state
 *
 * Return:
 * Mask of link configuration indices allowed after applying the current
 * maximum link limits, and further narrowing them by any forced link
 * parameters. The caller may further filter the returned mask before passing
 * it to the for_each_dp_link_config() iterators.
 *
 * The returned mask uses the canonical configuration index/mask scheme
 * shared by the link-caps API.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * See also:
 * - intel_dp_link_caps_set_max_limits()
 * - intel_dp_link_caps_forced_params()
 */
u32 intel_dp_link_caps_allowed_config_mask(struct intel_dp_link_caps *link_caps)
{
	return get_effective_config_mask(link_caps) &
	       ~link_caps->config_table.disabled_config_mask;
}

/**
 * intel_dp_link_caps_config_at - get config for a given config index
 * @link_caps: link capabilities state
 * @config_idx: configuration index to look up
 * @config: returned link configuration
 *
 * Look up the link configuration identified by @config_idx.
 *
 * @config_idx uses the canonical configuration index/mask scheme
 * shared by the link-caps API.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - %true if @config_idx is valid, storing the configuration in @config.
 * - %false if @config_idx is invalid.
 */
bool intel_dp_link_caps_config_at(struct intel_dp_link_caps *link_caps, int config_idx,
				  struct intel_dp_link_config *config)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	int iter_pos = config_idx;
	bool found;

	/* The config's index matches the config's BW / ASC iteration order position. */
	found = intel_dp_link_caps_config_iter_at(link_caps,
						  bw_asc_config_order(),
						  iter_pos,
						  config, &config_idx);

	if (drm_WARN_ON(display->drm, !found))
		return false;

	if (drm_WARN_ON(display->drm, config_idx != iter_pos))
		return false;

	return true;
}

static bool within_ppm_kunits(int actual_k, int nominal_k, int ppm)
{
	int diff_k = abs(actual_k - nominal_k);

	return mul_u32_u32(diff_k, 1000) <= mul_u32_u32(nominal_k, ppm);
}

/**
 * intel_dp_link_caps_find_allowed_config - find a matching allowed config
 * @link_caps: link capabilities state
 * @link_config: link configuration to match
 * @match_type: requested match type
 *
 * Search the currently allowed link configurations for a match to
 * @link_config.
 *
 * The return index uses the canonical configuration index/mask scheme
 * shared by the link-caps API.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - Configuration index of the first matching allowed configuration.
 * - %-1 if no allowed configuration matches.
 */
int intel_dp_link_caps_find_allowed_config(struct intel_dp_link_caps *link_caps,
					   const struct intel_dp_link_config *link_config,
					   enum intel_dp_link_caps_config_match_type match_type)
{
	struct intel_dp_link_config iter_config;
	int iter_config_idx;

	for_each_dp_link_config_idx(link_caps, bw_asc_config_order(),
				    intel_dp_link_caps_allowed_config_mask(link_caps),
				    &iter_config, &iter_config_idx) {
		/*
		 * link_config->rate may be platform-derived rather than the nominal
		 * supported link rate.
		 *
		 * When the caller requests fuzzy rate matching, accept a nominal rate
		 * within 10000ppm of the requested rate.
		 *
		 * The DP spec seems to allow at most 300ppm of symbol clock tolerance,
		 * excluding SSC. However, at least on g4x the 2.7Gbps rate exceeds that
		 * (~5000ppm), see intel_dp_compute_rate().
		 *
		 * The first match is also the best one, since nominal rates are guaranteed
		 * to be spaced much farther apart than 10000ppm.
		 *
		 * TODO: Track the nominal link rate separately, pass it here, and require
		 * an exact match.
		 */
		if (iter_config.rate != link_config->rate &&
		    (match_type == INTEL_DP_LINK_CAPS_CONFIG_MATCH_EXACT ||
		     !within_ppm_kunits(link_config->rate, iter_config.rate, 10000)))
			continue;

		if (iter_config.lane_count != link_config->lane_count)
			continue;

		return iter_config_idx;
	}

	return -1;
}

static int get_max_config(struct intel_dp_link_caps *link_caps,
			  enum intel_dp_link_caps_config_order_key order_key,
			  u32 config_mask,
			  struct intel_dp_link_config *config)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	struct intel_dp_link_caps_config_order order = {
		.key = order_key,
		.dir = INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_DESC
	};
	int config_idx;

	for_each_dp_link_config_idx(link_caps, order, config_mask, config, &config_idx)
		break;

	drm_WARN_ON(display->drm, config_idx < 0);

	return config_idx;
}

/**
 * intel_dp_link_caps_max_config - get the maximum config in a given order
 * @link_caps: link capabilities state
 * @order_key: ordering key used to choose the maximum config
 * @config_mask: mask of candidate configurations
 * @max_config: returned maximum link configuration
 *
 * Return the maximum configuration from @config_mask according to @order_key,
 * storing it in @max_config.
 *
 * @config_mask uses the canonical configuration index/mask scheme shared by
 * the link-caps API.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * See also:
 * - &enum intel_dp_link_caps_config_order_key
 */
void intel_dp_link_caps_max_config(struct intel_dp_link_caps *link_caps,
				   enum intel_dp_link_caps_config_order_key order_key,
				   u32 config_mask,
				   struct intel_dp_link_config *max_config)
{
	get_max_config(link_caps, order_key, config_mask, max_config);
}

/**
 * intel_dp_link_caps_max_config_idx - get the index of the maximum config
 * @link_caps: link capabilities state
 * @order_key: ordering key used to choose the maximum config
 * @config_mask: mask of candidate configurations
 *
 * Find the maximum configuration from @config_mask according to @order_key.
 *
 * @config_mask and the returned config index use the canonical configuration
 * index/mask scheme shared by the link-caps API.
 *
 * The caller must serialize this call against concurrent updates to
 * @link_caps.
 *
 * Return:
 * - Configuration index of the maximum matching configuration.
 * - %-1 if no configuration is selected by @config_mask.
 *
 * See also:
 * - &enum intel_dp_link_caps_config_order_key
 */
int intel_dp_link_caps_max_config_idx(struct intel_dp_link_caps *link_caps,
				      enum intel_dp_link_caps_config_order_key order_key,
				      u32 config_mask)
{
	struct intel_dp_link_config config;

	return get_max_config(link_caps, order_key, config_mask, &config);
}

static void set_max_link_limit_no_update(struct intel_dp_link_caps *link_caps,
					 const struct intel_dp_link_config *new_link_limits)
{
	link_caps->max_limits = *new_link_limits;
}

/* Compute max link limits from the currently allowed config mask. */
static void compute_max_link_limits(struct intel_dp_link_caps *link_caps,
				    struct intel_dp_link_config *max_link_limits)
{
	struct intel_dp_link_config link_config;

	*max_link_limits = INTEL_DP_LINK_CONFIG_NULL;
	for_each_dp_link_config(link_caps, bw_asc_config_order(),
				intel_dp_link_caps_allowed_config_mask(link_caps),
				&link_config) {
		max_link_limits->rate = max(max_link_limits->rate,
					    link_config.rate);
		max_link_limits->lane_count = max(max_link_limits->lane_count,
						  link_config.lane_count);
	}
}

static void reset_max_link_limit_no_update(struct intel_dp_link_caps *link_caps)
{
	link_caps->max_limits.rate = intel_dp_link_caps_max_common_rate(link_caps);
	link_caps->max_limits.lane_count = intel_dp_link_caps_max_common_lane_count(link_caps);
}

static void clear_all_restrictions(struct intel_dp_link_caps *link_caps)
{
	/*
	 * Try to recover by enabling all the configs and disabling all
	 * limits.
	 */
	link_caps->config_table.disabled_config_mask = 0;
	link_caps->forced_params = INTEL_DP_LINK_CONFIG_NULL;
	reset_max_link_limit_no_update(link_caps);
}

static bool assert_has_allowed_config(struct intel_dp_link_caps *link_caps)
{
	struct intel_display *display = to_intel_display(link_caps->dp);

	return !drm_WARN_ON(display->drm,
			    !intel_dp_link_caps_allowed_config_mask(link_caps));
}

/*
 * Recompute max_limits from the currently allowed configs.
 *
 * This can only reduce the current cached limits, by excluding
 * parameters that belong only to disabled configs or, when forcing is
 * active, configs outside the forced rate/lane constraints.
 *
 * Although the allowed mask itself depends on max_limits, this update
 * must not change the effective allowed set, since the recomputed limits
 * still cover every config that is currently allowed.
 */
static bool update_max_link_limits(struct intel_dp_link_caps *link_caps)
{
	u32 old_allowed_mask = intel_dp_link_caps_allowed_config_mask(link_caps);
	struct intel_dp_link_config new_limits;

	if (!assert_has_allowed_config(link_caps))
		return false;

	compute_max_link_limits(link_caps, &new_limits);

	/*
	 * The limits could only decrease due to disabled or forced
	 * configs.
	 */
	if (new_limits.rate > link_caps->max_limits.rate ||
	    new_limits.lane_count > link_caps->max_limits.lane_count)
		return false;

	/*
	 * The allowed mask shouldn't have changed, since the bounds could
	 * only get updated due to configs that were already disabled in the
	 * old mask. So the new limit values will not disable any configs.
	 */
	link_caps->max_limits = new_limits;
	if (old_allowed_mask != intel_dp_link_caps_allowed_config_mask(link_caps))
		return false;

	return true;
}

static bool update_max_link_info(struct intel_dp_link_caps *link_caps)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	bool limit_update_ok;

	limit_update_ok = update_max_link_limits(link_caps);

	if (drm_WARN_ON(display->drm, !limit_update_ok))
		clear_all_restrictions(link_caps);

	/* Align max BW config to the allowed config mask. */
	intel_dp_link_caps_max_config(link_caps,
				      INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW,
				      intel_dp_link_caps_allowed_config_mask(link_caps),
				      &link_caps->max_bw_config);

	return limit_update_ok;
}

static bool max_link_limits_valid(struct intel_dp_link_caps *link_caps,
				  const struct intel_dp_link_config *link_limits)
{
	struct intel_dp_link_config forced_params;

	intel_dp_link_caps_forced_params(link_caps, &forced_params);

	if (link_limits->rate < forced_params.rate ||
	    link_limits->lane_count < forced_params.lane_count)
		return false;

	return true;
}

/**
 * intel_dp_link_caps_set_max_limits - set the current maximum link limits
 * @link_caps: link capabilities state
 * @new_link_limits: new maximum link limits
 *
 * Set the current maximum rate and lane-count limits to @new_link_limits,
 * thus limiting the set of allowed configurations, and update the derived
 * maximum-link information accordingly.
 *
 * The limits must remain compatible with, i.e. not below, the effective
 * forced parameters returned by intel_dp_link_caps_forced_params().
 *
 * The caller must serialize this call against concurrent queries and
 * updates to @link_caps.
 *
 * Return:
 * - %true if @new_link_limits were accepted and the derived state was updated.
 * - %false if @new_link_limits are invalid or the derived state could not be
 *   updated.
 *
 * See also:
 * - intel_dp_link_caps_allowed_config_mask()
 */
bool intel_dp_link_caps_set_max_limits(struct intel_dp_link_caps *link_caps,
				      const struct intel_dp_link_config *new_link_limits)
{
	struct intel_display *display = to_intel_display(link_caps->dp);

	/* The caller must keep the max limits above the forced parameters. */
	if (drm_WARN_ON(display->drm, !max_link_limits_valid(link_caps, new_link_limits)))
		return false;

	set_max_link_limit_no_update(link_caps, new_link_limits);

	return update_max_link_info(link_caps);
}

/**
 * intel_dp_link_caps_get_max_limits - get the current maximum link limits
 * @link_caps: link capabilities state
 * @max_link_limits: returned maximum link limits
 *
 * Return the current maximum rate and lane-count limits in
 * @max_link_limits.
 *
 * These limits constrain the set of allowed configurations.
 *
 * The limits are set to the maximum common supported values after
 * intel_dp_link_caps_reset() is called, and can later be modified by
 * intel_dp_link_caps_set_max_limits(). The max rate and lane-count
 * parameters are independent limits, so the pair does not necessarily
 * define a valid configuration.
 *
 * This function may be called without serializing against updates to
 * @link_caps. However, without such serialization the returned value may be
 * a torn (link rate, lane count) tuple, i.e. the parameters may belong to
 * different update snapshots in time.
 */
void intel_dp_link_caps_get_max_limits(struct intel_dp_link_caps *link_caps,
				       struct intel_dp_link_config *max_link_limits)
{
	*max_link_limits = link_caps->max_limits;
}

/**
 * intel_dp_link_caps_reset_max_limits - reset the current maximum link limits
 * @link_caps: link capabilities state
 *
 * Reset the current maximum link limits to the maximum supported common link
 * rate and lane count, then update the derived maximum-link information
 * accordingly.
 *
 * The caller must serialize this call against concurrent queries and
 * updates to @link_caps.
 */
void intel_dp_link_caps_reset_max_limits(struct intel_dp_link_caps *link_caps)
{
	reset_max_link_limit_no_update(link_caps);
	update_max_link_info(link_caps);
}

/**
 * intel_dp_link_caps_max_bw_config - get the maximum-bandwidth allowed config
 * @link_caps: link capabilities state
 * @max_bw_config: returned maximum-bandwidth configuration
 *
 * Return the currently allowed configuration with the highest bandwidth in
 * @max_bw_config.
 *
 * This function may be called without serializing against updates to
 * @link_caps. However, without such serialization the returned value may be
 * a torn (link rate, lane count) tuple, i.e. the parameters may belong to
 * different update snapshots in time.
 */
void intel_dp_link_caps_max_bw_config(struct intel_dp_link_caps *link_caps,
				      struct intel_dp_link_config *max_bw_config)
{
	*max_bw_config = link_caps->max_bw_config;
}

static bool assert_config_idx_is_valid(struct intel_dp_link_caps *link_caps, int config_idx)
{
	struct intel_display *display = to_intel_display(link_caps->dp);

	return !drm_WARN_ON(display->drm,
			    !in_range(config_idx, 0, link_caps->config_table.num_configs));
}


/**
 * intel_dp_link_caps_disable_config - disable a configuration
 * @link_caps: link capabilities state
 * @config_idx: configuration index to disable
 *
 * Disable the configuration identified by @config_idx and update the derived
 * maximum-link information accordingly. This removes the configuration from
 * the set of allowed configurations.
 *
 * The configuration remains disallowed until intel_dp_link_caps_reset() is
 * called, which normally happens after a connector disconnect, or until some
 * error condition triggers recovery because the set of allowed
 * configurations would otherwise become empty.
 *
 * @config_idx uses the canonical configuration index/mask scheme shared by
 * the link-caps API.
 *
 * The caller must serialize this call against concurrent queries and
 * updates to @link_caps.
 *
 * Return:
 * - %true if @config_idx was valid and the derived state was updated.
 * - %false if @config_idx was invalid or the derived state could not be
 *   updated.
 */
bool intel_dp_link_caps_disable_config(struct intel_dp_link_caps *link_caps, int config_idx)
{
	if (!assert_config_idx_is_valid(link_caps, config_idx))
		return false;

	link_caps->config_table.disabled_config_mask |= BIT(config_idx);

	return update_max_link_info(link_caps);
}

static void enable_link_config_no_update(struct intel_dp_link_caps *link_caps, int config_idx)
{
	if (!assert_config_idx_is_valid(link_caps, config_idx))
		return;

	link_caps->config_table.disabled_config_mask &= ~BIT(config_idx);
}

static void sanitize_disallowed_config(struct intel_dp_link_caps *link_caps)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	struct intel_dp_link_config min_link_config;

	if (intel_dp_link_caps_allowed_config_mask(link_caps))
		return;

	drm_dbg_kms(display->drm,
		    "No allowed link config left, force enable the minimum config\n");

	if (!intel_dp_link_caps_config_at(link_caps, 0, &min_link_config))
		return;

	enable_link_config_no_update(link_caps, 0);
	reset_max_link_limit_no_update(link_caps);
}

static int intel_dp_link_config_bw(const struct intel_dp_link_config link_config)
{
	return drm_dp_max_dprx_data_rate(link_config.rate, link_config.lane_count);
}

static int link_config_cmp_by_bw(const void *a, const void *b, const void *p)
{
	const struct intel_dp_link_caps_config_table *table = p;
	const struct intel_dp_link_config_entry *lc_a =
		(const struct intel_dp_link_config_entry *)a;
	const struct intel_dp_link_config_entry *lc_b =
		(const struct intel_dp_link_config_entry *)b;
	struct intel_dp_link_config link_config_a;
	struct intel_dp_link_config link_config_b;
	int bw_a;
	int bw_b;

	to_intel_dp_link_config(table, lc_a, &link_config_a);
	to_intel_dp_link_config(table, lc_b, &link_config_b);

	bw_a = intel_dp_link_config_bw(link_config_a);
	bw_b = intel_dp_link_config_bw(link_config_b);

	if (bw_a != bw_b)
		return bw_a - bw_b;

	return link_config_a.rate - link_config_b.rate;
}

static int link_config_cmp_by_rate_lane_count(const void *a, const void *b, const void *p)
{
	const struct intel_dp_link_caps_config_table *table = p;
	u8 a_idx = *(const u8 *)a;
	u8 b_idx = *(const u8 *)b;
	const struct intel_dp_link_config_entry *lc_a =
		&table->configs[a_idx];
	const struct intel_dp_link_config_entry *lc_b =
		&table->configs[b_idx];
	struct intel_dp_link_config link_config_a;
	struct intel_dp_link_config link_config_b;

	to_intel_dp_link_config(table, lc_a, &link_config_a);
	to_intel_dp_link_config(table, lc_b, &link_config_b);

	if (link_config_a.rate != link_config_b.rate)
		return link_config_a.rate - link_config_b.rate;

	return link_config_a.lane_count - link_config_b.lane_count;
}

static bool build_config_table(struct intel_display *display,
			       const int *rates, int num_rates,
			       int max_lane_count,
			       struct intel_dp_link_caps_config_table *table)
{
	/* TODO: rename lc to config_entry for consistency. */
	struct intel_dp_link_config_entry *lc;
	int num_lane_configs;
	u8 *rate_lane_idx;
	int i;
	int j;

	if (drm_WARN_ON(display->drm,
			!in_range(num_rates, 1, ARRAY_SIZE(table->rates))))
		return false;

	if (drm_WARN_ON(display->drm, !is_power_of_2(max_lane_count)))
		return false;

	num_lane_configs = ilog2(max_lane_count) + 1;

	if (drm_WARN_ON(display->drm, num_rates * num_lane_configs >
				      ARRAY_SIZE(table->configs)))
		return false;

	memset(table, 0, sizeof(*table));

	table->max_lane_count = max_lane_count;

	memcpy(table->rates, rates, sizeof(rates[0]) * num_rates);
	table->num_rates = num_rates;

	table->num_configs = num_rates * num_lane_configs;

	lc = table->configs;
	for (i = 0; i < num_rates; i++) {
		for (j = 0; j < num_lane_configs; j++) {
			lc->lane_count_exp = j;
			lc->link_rate_idx = i;

			lc++;
		}
	}

	sort_r(table->configs,
	       table->num_configs,
	       sizeof(table->configs[0]),
	       link_config_cmp_by_bw, NULL,
	       table);

	rate_lane_idx = table->rate_lane_order_idx;
	for (i = 0; i < table->num_configs; i++)
		*rate_lane_idx++ = i;

	sort_r(table->rate_lane_order_idx,
	       table->num_configs,
	       sizeof(table->rate_lane_order_idx[0]),
	       link_config_cmp_by_rate_lane_count, NULL,
	       table);

	return true;
}

static int
lookup_config_table_entry(const struct intel_dp_link_caps_config_table *table,
			  int rate, int lane_count_exp)
{
	int rate_idx;
	int i;

	rate_idx = intel_dp_rate_index(table->rates, table->num_rates, rate);
	if (rate_idx < 0)
		return rate_idx;

	for (i = 0; i < table->num_configs; i++)
		if (table->configs[i].link_rate_idx == rate_idx &&
		    table->configs[i].lane_count_exp == lane_count_exp)
			return i;

	return -1;
}

/*
 * Look up the config entries in @from_table selected by @from_config_mask
 * in @to_table and return a mask of the matching entries there.
 *
 * Each bit in the returned mask indexes an entry in @to_table, so this
 * effectively remaps @from_config_mask from @from_table to @to_table.
 */
static u32
remap_config_mask_to_table(const struct intel_dp_link_caps_config_table *from_table,
			   u32 from_config_mask,
			   const struct intel_dp_link_caps_config_table *to_table)
{
	u32 to_config_mask = 0;
	int i;

	for (i = 0; i < from_table->num_configs; i++) {
		const struct intel_dp_link_config_entry *from_entry =
			&from_table->configs[i];
		int from_rate;
		int to_idx;

		if (!(BIT(i) & from_config_mask))
			continue;

		from_rate = link_config_entry_rate(from_table, from_entry);

		to_idx = lookup_config_table_entry(to_table,
						   from_rate,
						   from_entry->lane_count_exp);
		if (to_idx < 0)
			continue;

		to_config_mask |= BIT(to_idx);
	}

	return to_config_mask;
}

static bool set_config_table(struct intel_dp_link_caps *link_caps,
			     const struct intel_dp_link_caps_config_table *table,
			     u32 disabled_config_mask)
{
	link_caps->config_table = *table;
	link_caps->config_table.disabled_config_mask = disabled_config_mask;

	sanitize_disallowed_config(link_caps);

	return update_max_link_info(link_caps);
}

/**
 * intel_dp_link_caps_update - rebuild the supported link configuration state
 * @link_caps: link capabilities state
 * @rates: supported common link rates
 * @num_rates: number of entries in @rates
 * @max_lane_count: supported maximum lane count
 *
 * Rebuild the supported link-configuration table from @rates and
 * @max_lane_count, preserving the disabled state of configurations that still
 * exist in the rebuilt table.
 *
 * This function is called regularly, at least after a sink is connected, but
 * it may also be called later whenever the sink capabilities may have changed,
 * for example in response to HPD IRQ / RX_CAP_CHANGED signaling.
 *
 * In the Intel driver this function is currently called whenever the connector
 * detect handler runs, after reading the sink capabilities. This may change if
 * those capabilities are cached until the sink is disconnected, or until
 * RX_CAP_CHANGED is signaled. In any case, this function should be called
 * whenever the sink capabilities were read out and may have changed.
 *
 * Configurations that are no longer supported are dropped, newly supported
 * configurations are added enabled, and the derived maximum-link information
 * is updated to match the rebuilt table.
 *
 * Users of the link-caps API should not cache config indices across calls to
 * this function, since old indices may no longer refer to the same
 * configurations in the rebuilt table if configurations were removed or
 * added.
 *
 * The caller must serialize this call against concurrent queries and
 * updates to @link_caps.
 */
void intel_dp_link_caps_update(struct intel_dp_link_caps *link_caps,
				 const int *rates, int num_rates,
				 int max_lane_count)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	struct intel_dp_link_caps_config_table new_table;
	int new_disabled_config_mask;

	if (!build_config_table(display, rates, num_rates,
				max_lane_count, &new_table))
		return;

	/*
	 * Capture the currently disabled configs remapped to the new
	 * supported rates and max lane count so their disabled state can be
	 * restored in the rebuilt table.
	 *
	 * This must be called before setting the new table in link_caps.
	 */
	new_disabled_config_mask =
		remap_config_mask_to_table(&link_caps->config_table,
					   link_caps->config_table.disabled_config_mask,
					   &new_table);

	 /*
	  * Ignore the failure. Even on failure, set_config_table() keeps the new
	  * sink caps and leaves @link_caps in a sane state.
	  */
	set_config_table(link_caps, &new_table, new_disabled_config_mask);
}

/**
 * intel_dp_link_caps_reset - reset link-capability restrictions
 * @link_caps: link capabilities state
 *
 * Reset all current restrictions except for the user-requested forced
 * parameters, thus updating the set of allowed configurations and the
 * derived maximum-link information accordingly.
 *
 * This function is regularly called after a sink is connected, either for the
 * first time to the connector or after a previous sink was disconnected from
 * it, and intel_dp_link_caps_update() was called.
 *
 * The caller must serialize this call against concurrent queries and
 * updates to @link_caps.
 */
void intel_dp_link_caps_reset(struct intel_dp_link_caps *link_caps)
{
	/* Reset all restrictions except for the user-requested forced config. */
	link_caps->config_table.disabled_config_mask = 0;
	reset_max_link_limit_no_update(link_caps);

	update_max_link_info(link_caps);
}

static int i915_dp_force_link_rate_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps =
		connector_to_dp_link_caps(connector);
	struct intel_dp *intel_dp = link_caps->dp;
	struct intel_dp_link_config forced_link_params;
	struct intel_dp_link_config active_link_config;
	int active_link_rate = -1;
	int err;
	int i;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	if (intel_dp_link_active_config(intel_dp, &active_link_config))
		active_link_rate = active_link_config.rate;

	forced_link_params = link_caps->forced_params;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	seq_printf(m, "%sauto%s",
		   forced_link_params.rate == 0 ? "[" : "",
		   forced_link_params.rate == 0 ? "]" : "");

	for (i = 0; i < intel_dp->num_source_rates; i++)
		seq_printf(m, " %s%d%s%s",
			   intel_dp->source_rates[i] == forced_link_params.rate ? "[" : "",
			   intel_dp->source_rates[i],
			   intel_dp->source_rates[i] == active_link_rate ? "*" : "",
			   intel_dp->source_rates[i] == forced_link_params.rate ? "]" : "");

	seq_putc(m, '\n');

	return 0;
}

static int parse_link_rate(struct intel_dp *intel_dp, const char __user *ubuf, size_t len)
{
	char *kbuf;
	const char *p;
	int rate;
	int ret = 0;

	kbuf = memdup_user_nul(ubuf, len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	p = strim(kbuf);

	if (!strcmp(p, "auto")) {
		rate = 0;
	} else {
		ret = kstrtoint(p, 0, &rate);
		if (ret < 0)
			goto out_free;

		if (intel_dp_rate_index(intel_dp->source_rates,
					intel_dp->num_source_rates,
					rate) < 0)
			ret = -EINVAL;
	}

out_free:
	kfree(kbuf);

	return ret < 0 ? ret : rate;
}

static ssize_t i915_dp_force_link_rate_write(struct file *file,
					     const char __user *ubuf,
					     size_t len, loff_t *offp)
{
	struct seq_file *m = file->private_data;
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps = connector_to_dp_link_caps(connector);
	struct intel_dp *intel_dp = link_caps->dp;
	int rate;
	int err;

	rate = parse_link_rate(intel_dp, ubuf, len);
	if (rate < 0)
		return rate;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	intel_dp_reset_link_params(intel_dp);
	/* Should succeed after reset restored the sink maximum limits. */
	if (drm_WARN_ON(display->drm, !set_forced_link_rate(link_caps, rate)))
		err = -EINVAL;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	if (err)
		return err;

	*offp += len;

	return len;
}
DEFINE_SHOW_STORE_ATTRIBUTE(i915_dp_force_link_rate);

static int i915_dp_force_lane_count_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps = connector_to_dp_link_caps(connector);
	struct intel_dp *intel_dp = link_caps->dp;
	struct intel_dp_link_config forced_link_params;
	struct intel_dp_link_config active_link_config;
	int active_lane_count = -1;
	int err;
	int i;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	if (intel_dp_link_active_config(intel_dp, &active_link_config))
		active_lane_count = active_link_config.lane_count;

	forced_link_params = link_caps->forced_params;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	seq_printf(m, "%sauto%s",
		   forced_link_params.lane_count == 0 ? "[" : "",
		   forced_link_params.lane_count == 0 ? "]" : "");

	for (i = 1; i <= 4; i <<= 1)
		seq_printf(m, " %s%d%s%s",
			   i == forced_link_params.lane_count ? "[" : "",
			   i,
			   i == active_lane_count ? "*" : "",
			   i == forced_link_params.lane_count ? "]" : "");

	seq_putc(m, '\n');

	return 0;
}

static int parse_lane_count(const char __user *ubuf, size_t len)
{
	char *kbuf;
	const char *p;
	int lane_count;
	int ret = 0;

	kbuf = memdup_user_nul(ubuf, len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	p = strim(kbuf);

	if (!strcmp(p, "auto")) {
		lane_count = 0;
	} else {
		ret = kstrtoint(p, 0, &lane_count);
		if (ret < 0)
			goto out_free;

		switch (lane_count) {
		case 1:
		case 2:
		case 4:
			break;
		default:
			ret = -EINVAL;
		}
	}

out_free:
	kfree(kbuf);

	return ret < 0 ? ret : lane_count;
}

static ssize_t i915_dp_force_lane_count_write(struct file *file,
					      const char __user *ubuf,
					      size_t len, loff_t *offp)
{
	struct seq_file *m = file->private_data;
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps = connector_to_dp_link_caps(connector);
	struct intel_dp *intel_dp = link_caps->dp;
	int lane_count;
	int err;

	lane_count = parse_lane_count(ubuf, len);
	if (lane_count < 0)
		return lane_count;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	intel_dp_reset_link_params(intel_dp);
	/* Should succeed after reset restored the sink maximum limits. */
	if (drm_WARN_ON(display->drm, !set_forced_lane_count(link_caps, lane_count)))
		err = -EINVAL;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	if (err)
		return err;

	*offp += len;

	return len;
}
DEFINE_SHOW_STORE_ATTRIBUTE(i915_dp_force_lane_count);

static int i915_dp_max_link_rate_show(void *data, u64 *val)
{
	struct intel_connector *connector = to_intel_connector(data);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps =
		connector_to_dp_link_caps(connector);
	struct intel_dp_link_config max_link_limits;
	int err;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	intel_dp_link_caps_get_max_limits(link_caps, &max_link_limits);
	*val = max_link_limits.rate;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(i915_dp_max_link_rate_fops, i915_dp_max_link_rate_show, NULL, "%llu\n");

static int i915_dp_max_lane_count_show(void *data, u64 *val)
{
	struct intel_connector *connector = to_intel_connector(data);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps =
		connector_to_dp_link_caps(connector);
	struct intel_dp_link_config max_link_limits;
	int err;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	intel_dp_link_caps_get_max_limits(link_caps, &max_link_limits);
	*val = max_link_limits.lane_count;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(i915_dp_max_lane_count_fops, i915_dp_max_lane_count_show, NULL, "%llu\n");

static int intel_dp_allowed_link_configs_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp_link_caps *link_caps =
		connector_to_dp_link_caps(connector);
	struct intel_dp_link_caps_config_order config_order =
		intel_dp_link_caps_config_order_for_connector(connector);
	struct intel_dp_link_config link_config;
	u32 config_mask;
	int err;
	int i;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	config_mask = intel_dp_link_caps_allowed_config_mask(link_caps);

	i = 0;
	for_each_dp_link_config(link_caps, config_order, config_mask, &link_config) {
		seq_printf(m, "%s%dx%d",
			   i ? " " : "",
			   link_config.lane_count, link_config.rate);
		i++;
	}

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	seq_putc(m, '\n');

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(intel_dp_allowed_link_configs);

/**
 * intel_dp_link_caps_debugfs_add - add link caps debugfs files for a connector
 * @connector: connector to add the debugfs files for
 *
 * Add the link-capability debugfs files for a DP @connector.
 */
void intel_dp_link_caps_debugfs_add(struct intel_connector *connector)
{
	struct dentry *root = connector->base.debugfs_entry;

	if (connector->base.connector_type != DRM_MODE_CONNECTOR_DisplayPort &&
	    connector->base.connector_type != DRM_MODE_CONNECTOR_eDP)
		return;

	debugfs_create_file("i915_dp_force_link_rate", 0644, root,
			    connector, &i915_dp_force_link_rate_fops);

	debugfs_create_file("i915_dp_force_lane_count", 0644, root,
			    connector, &i915_dp_force_lane_count_fops);

	debugfs_create_file("i915_dp_max_link_rate", 0444, root,
			    connector, &i915_dp_max_link_rate_fops);

	debugfs_create_file("i915_dp_max_lane_count", 0444, root,
			    connector, &i915_dp_max_lane_count_fops);

	debugfs_create_file("intel_dp_allowed_link_configs", 0444, root,
			    connector, &intel_dp_allowed_link_configs_fops);
}

/**
 * intel_dp_link_caps_init - allocate and initialize link caps state
 * @dp: DP encoder state
 *
 * Allocate and initialize the link capabilities state for @dp and the
 * connectors attached to it.
 *
 * Return:
 * - Pointer to the newly allocated link capabilities state.
 * - %NULL if allocation fails.
 */
struct intel_dp_link_caps *intel_dp_link_caps_init(struct intel_dp *dp)
{
	struct intel_dp_link_caps *link_caps;

	link_caps = kzalloc_obj(*link_caps);
	if (!link_caps)
		return NULL;

	link_caps->dp = dp;

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
