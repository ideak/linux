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
#include "intel_display_utils.h"
#include "intel_dp.h"
#include "intel_dp_link_caps.h"

/**
 * DOC: DisplayPort link capabilities
 *
 * The Intel DP link caps API tracks the supported and allowed DP link
 * configurations for a DP encoder and its attached connectors, and
 * provides helpers to iterate, filter, disable, and constrain them.
 *
 * Locking:
 *   All accesses to this API must be serialized. The only exception
 *   is intel_dp_link_caps_get_max_limits(), which allow lockless
 *   lookup. Such lookups may observe an out-of-sync &struct
 *   intel_dp_link_config tuple, i.e. a rate from one state and a lane
 *   count from another.
 *
 *   The Intel i915/xe drivers ensure the above serialization by holding
 *   &drm_mode_config.connection_mutex and, while holding the lock,
 *   flushing pending asynchronous atomic commits. This also allows use
 *   of the API from the tails of asynchronous atomic commits, which
 *   cannot hold the lock.
 *
 * Configuration indexing and iteration position:
 *   A configuration index or mask always refers to the same
 *   configuration or set of configurations across all API calls.
 *
 *   In contrast, an iteration position depends on the selected
 *   configuration ordering (key and direction). Any mapping between
 *   iteration positions and configuration indices is
 *   ordering-dependent and not part of the API, and must not be relied
 *   upon.
 *
 *   Configuration indices are not stable across
 *   intel_dp_link_caps_update() calls. API users must not cache
 *   configuration indices or masks across such updates.
 *
 *   The API also supports iterating configurations in ascending and
 *   descending BW order, and in ascending and descending rate/lane order.
 *   The for_each_dp_link_config*() helpers iterate configurations in
 *   these orders.
 *
 * Terminology:
 *   "Common link capabilities" (or "common caps") refer to the link
 *   rates and maximum lane count supported by both the source and the
 *   sink, i.e. the intersection of their respective capabilities.
 *
 *   "Supported configurations" are all configurations defined by the
 *   common link capabilities' link rates and maximum lane count.
 *
 *   "Disabled configurations" are supported configurations disabled via
 *   this API.
 *
 *   "Enabled configurations" are supported configurations that are not
 *   disabled.
 *
 *   "Forced configurations" are enabled configurations forced via
 *   debugfs.
 *
 *   "Allowed configurations" are the enabled configurations, or if
 *   forcing is in effect the forced configurations, constrained by a
 *   maximum rate and lane count set via the API.
 */
struct intel_dp_link_caps {
	struct intel_dp *dp;

	struct intel_dp_link_caps_config_table {
		/* Rate, lane count caps common to source and sink. */
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
			/* index into rates[] */
			u8 link_rate_idx:INTEL_DP_LINK_RATE_IDX_BITS;
			u8 lane_count_exp:INTEL_DP_LANE_COUNT_EXP_BITS;
		} configs[INTEL_DP_MAX_LINK_CONFIGS];
	} config_table;

	/*
	 * Forced parameters requested via debugfs. Remains set across sink
	 * disconnects.
	 */
	struct intel_dp_link_config forced_params;

	/*
	 * Cached / settable upper bounds of the allowed configurations.
	 *
	 * max_limits is set via the link caps API and kept updated to the
	 * actual maximal parameter bounds of the allowed configurations.
	 * Such updates do not increase max_limits above the value set via the
	 * API, except when max_limits is reset, see below. The limits (set
	 * via the API) will constrain the allowed configurations.
	 *
	 * max_limits is reset in the same cases as the disabled configuration
	 * mask (see config_table.disabled_config_mask). This restores
	 * max_limits to the maximum parameters of the allowed configurations
	 * as defined above, with the previous max_limits constraint removed
	 * and the disabled configuration mask cleared, i.e. constrained only
	 * by forced_params.
	 *
	 * max_limits.rate and max_limits.lane_count may come from different
	 * allowed configurations, i.e. the (max_limits.rate,
	 * max_limits.lane_count) tuple itself may not be an allowed
	 * configuration.
	 *
	 * TODO: List the events that reset max_limits, after the
	 * introduction of disabled_config_mask.
	 *
	 * TODO: Make max_limits reflect the maximum of the allowed
	 * configurations at all times and stop making it settable via the
	 * API, removing it as a constraint on the allowed configurations.
	 */
	struct intel_dp_link_config max_limits;
};

static int lookup_rate(const struct intel_dp_link_caps_config_table *table, int index)
{
	if (WARN_ON(index < 0 || index >= table->num_rates))
		return 162000;

	return table->rates[index];
}

/* Get length of common rates array potentially limited by max_rate. */
static int intel_dp_link_caps_common_len_rate_limit(struct intel_dp_link_caps *link_caps,
						    int max_rate)
{
	const struct intel_dp_link_caps_config_table *table = &link_caps->config_table;

	return intel_dp_rate_limit_len(table->rates, table->num_rates, max_rate);
}

/**
 * intel_dp_link_caps_common_rate - get common link rate at a given index
 * @link_caps: link capabilities state
 * @index: index into the common rate list
 *
 * Return the link rate identified by @idx currently supported by @link_caps,
 * common to both the source and the sink.
 *
 * Return:
 * - Common link rate at @idx.
 * - 162000 if @idx is out of range.
 */
int intel_dp_link_caps_common_rate(struct intel_dp_link_caps *link_caps, int index)
{
	return lookup_rate(&link_caps->config_table, index);
}

/**
 * intel_dp_link_caps_common_rate_idx - get index of a common link rate
 * @link_caps: link capabilities state
 * @rate: common link rate to look up
 *
 * Look up @rate in the rate list currently supported by @link_caps, common to
 * both the source and the sink.
 *
 * The returned value is an index into the common rate list returned by
 * intel_dp_link_caps_all_common_rates() and accepted by
 * intel_dp_link_caps_common_rate().
 *
 * Return:
 * - Index of @rate in the current common rate list.
 * - %-1 if @rate is not present.
 */
int intel_dp_link_caps_common_rate_idx(struct intel_dp_link_caps *link_caps, int rate)
{
	const struct intel_dp_link_caps_config_table *table = &link_caps->config_table;

	return intel_dp_rate_index(table->rates, table->num_rates, rate);
}

/**
 * intel_dp_link_caps_max_common_rate - get the maximum common link rate
 * @link_caps: link capabilities state
 *
 * Return:
 * Maximum link rate currently supported by @link_caps, common to both the
 * source and the sink.
 */
int intel_dp_link_caps_max_common_rate(struct intel_dp_link_caps *link_caps)
{
	const struct intel_dp_link_caps_config_table *table = &link_caps->config_table;

	return intel_dp_link_caps_common_rate(link_caps, table->num_rates - 1);
}

int intel_dp_link_caps_num_common_rates(struct intel_dp_link_caps *link_caps)
{
	return link_caps->config_table.num_rates;
}

/**
 * intel_dp_link_caps_all_common_rates - get all common link rates
 * @link_caps: link capabilities state
 * @rates: returned pointer to the common rate array
 * @num_rates: returned number of entries in @rates
 *
 * Return all link rates through @rates and @num_rates that are currently
 * supported by @link_caps, common to both the source and the sink. The
 * returned array is owned by @link_caps.
 *
 * Besides the usual locking requirement for API access, the caller must
 * also serialize any dereference of the returned array against concurrent
 * updates to @link_caps.
 */
void intel_dp_link_caps_all_common_rates(struct intel_dp_link_caps *link_caps,
					 const int **rates, int *num_rates)
{
	const struct intel_dp_link_caps_config_table *table = &link_caps->config_table;

	*rates = table->rates;
	*num_rates = table->num_rates;
}

static int intel_dp_link_caps_max_common_lane_count(struct intel_dp_link_caps *link_caps)
{
	return link_caps->config_table.max_lane_count;
}

static int forced_lane_count(struct intel_dp_link_caps *link_caps)
{
	if (!link_caps->forced_params.lane_count)
		return 0;

	return clamp(link_caps->forced_params.lane_count,
		     1, intel_dp_link_caps_max_common_lane_count(link_caps));
}

static int forced_link_rate(struct intel_dp_link_caps *link_caps)
{
	int len;

	if (!link_caps->forced_params.rate)
		return 0;

	len = intel_dp_link_caps_common_len_rate_limit(link_caps, link_caps->forced_params.rate);
	if (len == 0)
		return intel_dp_link_caps_common_rate(link_caps, 0);

	return intel_dp_link_caps_common_rate(link_caps, len - 1);
}

void intel_dp_link_caps_get_forced_params(struct intel_dp_link_caps *link_caps,
					  struct intel_dp_link_config *forced_params)
{
	forced_params->rate = forced_link_rate(link_caps);
	forced_params->lane_count = forced_lane_count(link_caps);
}

static int intel_dp_link_config_rate(const struct intel_dp_link_caps_config_table *table,
				     const struct intel_dp_link_config_entry *lc)
{
	return lookup_rate(table, lc->link_rate_idx);
}

static int intel_dp_link_config_lane_count(const struct intel_dp_link_config_entry *lc)
{
	return 1 << lc->lane_count_exp;
}

static int link_config_idx_to_rate(const struct intel_dp_link_caps_config_table *table,
				   int config_idx)
{
	const struct intel_dp_link_config_entry *lc = &table->configs[config_idx];

	return intel_dp_link_config_rate(table, lc);
}

static int link_config_idx_to_lane_count(const struct intel_dp_link_caps_config_table *table,
					 int config_idx)
{
	const struct intel_dp_link_config_entry *lc = &table->configs[config_idx];

	return intel_dp_link_config_lane_count(lc);
}

static void
to_intel_dp_link_config(const struct intel_dp_link_caps_config_table *table,
			int config_idx, struct intel_dp_link_config *config)
{
	config->rate = link_config_idx_to_rate(table, config_idx);
	config->lane_count = link_config_idx_to_lane_count(table, config_idx);
}

static bool
get_table_config_by_pos(const struct intel_dp_link_caps_config_table *config_table,
			struct intel_dp_link_caps_config_order config_order,
			int iter_pos,
			struct intel_dp_link_config *config, int *config_idx)
{
	if (!in_range(iter_pos, 0, config_table->num_configs))
		goto out_fail;

	switch (config_order.dir) {
	case INTEL_DP_LINK_CAPS_CONFIG_ORDER_DIR_ASC:
		break;
	default:
		MISSING_CASE(config_order.dir);

		goto out_fail;
	}

	switch (config_order.key) {
	case INTEL_DP_LINK_CAPS_CONFIG_ORDER_KEY_BW:
		*config_idx = iter_pos;

		break;
	default:
		MISSING_CASE(config_order.key);

		goto out_fail;
	}

	to_intel_dp_link_config(config_table, *config_idx, config);

	return true;

out_fail:
	*config = INTEL_DP_LINK_CONFIG_NULL;
	*config_idx = -1;

	return false;
}

static u32 calc_allowed_config_mask(struct intel_dp_link_caps *link_caps,
				    u32 disabled_config_mask,
				    const struct intel_dp_link_config *max_limits,
				    const struct intel_dp_link_config *forced_params)
{
	struct intel_dp_link_caps_config_table *table = &link_caps->config_table;
	struct intel_dp_link_config config;
	u32 allowed_mask = 0;
	int config_idx;

	for (config_idx = 0; config_idx < table->num_configs; config_idx++) {
		if (BIT(config_idx) & disabled_config_mask)
			continue;

		to_intel_dp_link_config(table, config_idx, &config);

		if (forced_params->rate &&
		    forced_params->rate != config.rate)
			continue;

		if (forced_params->lane_count &&
		    forced_params->lane_count != config.lane_count)
			continue;

		if (config.rate > max_limits->rate)
			continue;

		if (config.lane_count > max_limits->lane_count)
			continue;

		allowed_mask |= BIT(config_idx);
	}

	return allowed_mask;
}

/**
 * intel_dp_link_caps_get_allowed_config_mask - get the currently allowed config mask
 * @link_caps: link capabilities state
 *
 * Return:
 * Mask of link configuration indices allowed after applying the current
 * maximum link limits, and further narrowing them by any forced link
 * parameters. The caller may further filter the returned mask before passing
 * it to the for_each_dp_link_config() iterators.
 *
 * See also:
 * - intel_dp_link_caps_set_max_limits()
 * - intel_dp_link_caps_get_forced_params()
 */
u32 intel_dp_link_caps_get_allowed_config_mask(struct intel_dp_link_caps *link_caps)
{
	struct intel_dp_link_config forced_params;
	u32 disabled_mask = 0;	/* get the mask from link_caps. */

	intel_dp_link_caps_get_forced_params(link_caps, &forced_params);

	return calc_allowed_config_mask(link_caps, disabled_mask,
					&link_caps->max_limits, &forced_params);
}

/**
 * intel_dp_link_caps_get_config_by_pos - get config at a given iterator position
 * @link_caps: link capability state
 * @config_order: iteration order
 * @iter_pos: position in the @config_order iteration order
 * @config: returned link configuration
 * @config_idx: returned config index
 *
 * Look up the link config at iterator position @iter_pos in the order
 * described by @config_order.
 *
 * Note that any mapping between @iter_pos and @config_idx is an
 * implementation detail defined by @config_order and must not be
 * relied upon. The returned @config_idx always, regardless of
 * @config_order, uses the canonical configuration index/mask scheme
 * shared by the link-caps API.
 *
 * Return:
 * - %true  if @iter_pos is valid, storing the configuration in @config and
 *          its index in @config_idx.
 * - %false if @iter_pos is out of range, storing %INTEL_DP_LINK_CONFIG_NULL
 *          in @config and -1 in @config_idx.
 */
bool intel_dp_link_caps_get_config_by_pos(struct intel_dp_link_caps *link_caps,
					  struct intel_dp_link_caps_config_order config_order,
					  int iter_pos,
					  struct intel_dp_link_config *config, int *config_idx)
{
	return get_table_config_by_pos(&link_caps->config_table, config_order, iter_pos,
				       config, config_idx);
}

static bool is_within_percent(int actual, int nominal, int percent)
{
	int diff = abs(actual - nominal);

	if (WARN_ON(percent == 0 ||
		    diff > INT_MAX / 100 || nominal > INT_MAX / percent))
		return false;

	return diff * 100 <= nominal * percent;
}

static int
find_config_table_entry_pos(const struct intel_dp_link_caps_config_table *config_table,
			    struct intel_dp_link_caps_config_order config_order, u32 config_mask,
			    enum intel_dp_link_caps_config_match_type match_type,
			    const struct intel_dp_link_config *link_config)
{
	struct intel_dp_link_config iter_config;
	int iter_config_idx;
	int iter_pos;

	for (iter_pos = 0;
	     get_table_config_by_pos(config_table, config_order, iter_pos,
				     &iter_config, &iter_config_idx);
	     iter_pos++) {
		if (!(BIT(iter_config_idx) & config_mask))
			continue;

		if (iter_config.lane_count != link_config->lane_count)
			continue;

		/*
		 * link_config->rate may be platform-derived rather than the nominal
		 * supported link rate.
		 *
		 * When the caller requests fuzzy rate matching, accept a nominal rate
		 * within 1 percent of the requested rate.
		 *
		 * The DP spec seems to allow at most 300 ppm of symbol clock tolerance,
		 * excluding SSC. However, at least on g4x, the 2.7 Gbps rate exceeds
		 * that (~5000ppm); see intel_dp_compute_rate(). So allow 10000 ppm, or
		 * a 1 percent difference.
		 *
		 * The first match is also the best one, since nominal rates are guaranteed
		 * to be spaced much farther apart than 1 percent.
		 *
		 * TODO: Track the nominal link rate separately, pass it here, and require
		 * an exact match.
		 */
		if (iter_config.rate != link_config->rate &&
		    (match_type == INTEL_DP_LINK_CAPS_CONFIG_MATCH_EXACT ||
		     !is_within_percent(link_config->rate, iter_config.rate, 1)))
			continue;

		return iter_pos;
	}

	return -1;
}

/**
 * intel_dp_link_caps_find_allowed_config_pos - find matching allowed config position
 * @link_caps: link capabilities state
 * @config_order: iteration order
 * @match_type: requested match type
 * @link_config: link configuration to match
 *
 * Search the currently allowed link configurations for a match to
 * @link_config.
 *
 * Return:
 * - The position of the first matching allowed configuration in the
 *   @config_order iteration.
 * - %-1 if no allowed configuration matches.
 */
int intel_dp_link_caps_find_allowed_config_pos(struct intel_dp_link_caps *link_caps,
					       struct intel_dp_link_caps_config_order config_order,
					       enum intel_dp_link_caps_config_match_type match_type,
					       const struct intel_dp_link_config *link_config)
{
	u32 allowed_config_mask = intel_dp_link_caps_get_allowed_config_mask(link_caps);

	return find_config_table_entry_pos(&link_caps->config_table, config_order,
					   allowed_config_mask, match_type,
					   link_config);
}

static void set_max_link_limits_no_update(struct intel_dp_link_caps *link_caps,
					  const struct intel_dp_link_config *max_link_limits)
{
	link_caps->max_limits = *max_link_limits;
}

static void reset_max_link_limits_no_update(struct intel_dp_link_caps *link_caps)
{
	struct intel_dp_link_config max_link_limits = {
		.rate = intel_dp_link_caps_max_common_rate(link_caps),
		.lane_count = intel_dp_link_caps_max_common_lane_count(link_caps),
	};

	set_max_link_limits_no_update(link_caps, &max_link_limits);
}

static void reset_all_restrictions_no_update(struct intel_dp_link_caps *link_caps)
{
	reset_max_link_limits_no_update(link_caps);
	link_caps->forced_params = INTEL_DP_LINK_CONFIG_NULL;
}

/*
 * Compute the maximum link limits from the allowed configurations.
 *
 * The result reflects the maximum rate and lane count among the
 * configurations currently allowed by link_caps, i.e. constrained by
 * the currently stored max_limits, forced parameters. Since the
 * allowed set depends on max_limits, the result can only be less
 * than or equal to the current max_limits.
 */
static void compute_max_link_limits(struct intel_dp_link_caps *link_caps,
				    struct intel_dp_link_config *max_link_limits)
{
	struct intel_dp_link_caps_config_table *table = &link_caps->config_table;
	u32 allowed_mask = intel_dp_link_caps_get_allowed_config_mask(link_caps);
	struct intel_dp_link_config max_config = {};
	struct intel_dp_link_config link_config;
	int config_idx;

	for (config_idx = 0; config_idx < table->num_configs; config_idx++) {
		if (!(BIT(config_idx) & allowed_mask))
			continue;

		to_intel_dp_link_config(table, config_idx, &link_config);

		max_config.rate = max(max_config.rate,
				      link_config.rate);
		max_config.lane_count = max(max_config.lane_count,
					    link_config.lane_count);
	}

	*max_link_limits = max_config;
}

/**
 * update_max_link_limits - update max_limits to match the allowed configs
 * @link_caps: link capabilities state
 *
 * Adjust the stored max_limits to the actual parameter bounds of the
 * currently allowed configurations.
 *
 * The allowed configurations are already constrained by the stored
 * max_limits and forced parameters. The recomputed max_limits is
 * derived from that same set, so it can only be less than or equal to
 * the stored max_limits.
 *
 * A %false return indicates an internal error (either the stored
 * max_limits was below all allowed configurations, or
 * compute_max_link_limits() returned a larger value). The caller must
 * recover by removing all restrictions.
 *
 * Return:
 * - %true  if max_limits was updated successfully.
 * - %false if an internal error was detected.
 */
static bool update_max_link_limits(struct intel_dp_link_caps *link_caps)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	struct intel_dp_link_config new_limits;

	compute_max_link_limits(link_caps, &new_limits);

	if (drm_WARN_ON(display->drm,
			new_limits.rate == 0 ||
			new_limits.lane_count == 0 ||
			new_limits.rate > link_caps->max_limits.rate ||
			new_limits.lane_count > link_caps->max_limits.lane_count))
		return false;

	link_caps->max_limits = new_limits;

	return true;
}

static bool update_max_link_info(struct intel_dp_link_caps *link_caps)
{
	struct intel_display *display = to_intel_display(link_caps->dp);
	bool limit_update_ok;

	limit_update_ok = update_max_link_limits(link_caps);

	if (drm_WARN_ON(display->drm, !limit_update_ok))
		reset_all_restrictions_no_update(link_caps);

	return limit_update_ok;
}

/**
 * intel_dp_link_caps_get_max_limits - get the current maximum link limits
 * @link_caps: link capabilities state
 * @max_link_limits: returned maximum link limits
 *
 * Return the current maximum rate and lane count limits in
 * @max_link_limits.
 *
 * These limits constrain the set of allowed configurations.
 *
 * The limits are set to the maximum common supported values after
 * intel_dp_link_caps_reset() is called, and can later be modified by
 * intel_dp_link_caps_set_max_limits(). The max rate and lane count
 * parameters are independent limits, so the pair does not necessarily
 * define a valid configuration.
 *
 * This function may be called without serializing against updates to
 * @link_caps. However, without such serialization the returned value may be
 * an out-of-sync (link rate, lane count) tuple, i.e. the parameters may
 * belong to different update snapshots in time.
 */
void intel_dp_link_caps_get_max_limits(struct intel_dp_link_caps *link_caps,
				       struct intel_dp_link_config *max_link_limits)
{
	*max_link_limits = link_caps->max_limits;
}

static bool max_link_limits_valid(struct intel_dp_link_caps *link_caps,
				  const struct intel_dp_link_config *max_link_limits)
{
	struct intel_dp_link_config forced_params;
	u32 disabled_mask = 0;	/* get the mask from link_caps. */
	u32 allowed_mask;

	intel_dp_link_caps_get_forced_params(link_caps, &forced_params);
	allowed_mask = calc_allowed_config_mask(link_caps, disabled_mask,
						max_link_limits, &forced_params);

	return allowed_mask != 0;
}

/**
 * intel_dp_link_caps_set_max_limits - set the current maximum link limits
 * @link_caps: link capabilities state
 * @max_link_limits: new maximum link limits
 *
 * Set the current maximum rate and lane count limits to @max_link_limits,
 * after adjusting @max_link_limits to the currently allowed configuration
 * set. Since the old @max_link_limits also constrains this allowed set, the
 * new adjusted value of @max_link_limits will be at or below the old one.
 *
 * The new limits must leave at least one configuration allowed: the limits
 * must not be below the currently active forced parameters or below all the
 * configurations that remain after disabled configurations are excluded.
 *
 * Unlike intel_dp_link_caps_get_max_limits(), the caller must serialize
 * this call against concurrent queries and updates to @link_caps, in line
 * with the rest of the API.
 *
 * Return:
 * - %true  if the @link_caps cached max limits value got updated with
 *          @max_link_limits along with all the max link information.
 * - %false if @max_link_limits is invalid, or if max link info update
 *          fails due to an internal consistency issue. In the latter
 *          case return after resetting all limits and restrictions.
 */
bool intel_dp_link_caps_set_max_limits(struct intel_dp_link_caps *link_caps,
				       const struct intel_dp_link_config *max_link_limits)
{
	if (!max_link_limits_valid(link_caps, max_link_limits))
		return false;

	set_max_link_limits_no_update(link_caps, max_link_limits);

	return update_max_link_info(link_caps);
}

/**
 * intel_dp_link_caps_reset_max_limits - reset the current maximum link limits
 * @link_caps: link capabilities state
 *
 * Reset the current maximum link limits to the maximum supported common link
 * rate and lane count, then update the derived maximum-link information
 * accordingly.
 */
void intel_dp_link_caps_reset_max_limits(struct intel_dp_link_caps *link_caps)
{
	reset_max_link_limits_no_update(link_caps);
	/* On failure the following removes all restrictions. */
	update_max_link_info(link_caps);
}

static int intel_dp_link_config_bw(const struct intel_dp_link_caps_config_table *table,
				   const struct intel_dp_link_config_entry *lc)
{
	return drm_dp_max_dprx_data_rate(intel_dp_link_config_rate(table, lc),
					 intel_dp_link_config_lane_count(lc));
}

static int link_config_cmp_by_bw(const void *a, const void *b, const void *p)
{
	const struct intel_dp_link_caps_config_table *table = p;
	const struct intel_dp_link_config_entry *lc_a = a;
	const struct intel_dp_link_config_entry *lc_b = b;
	int bw_a = intel_dp_link_config_bw(table, lc_a);
	int bw_b = intel_dp_link_config_bw(table, lc_b);

	if (bw_a != bw_b)
		return bw_a - bw_b;

	return intel_dp_link_config_rate(table, lc_a) -
	       intel_dp_link_config_rate(table, lc_b);
}

static bool current_common_caps_match(struct intel_dp_link_caps_config_table *table,
				      const int *rates, int num_rates,
				      int old_max_lane_count)
{
	int current_max_lane_count = table->max_lane_count;
	const int *current_rates = table->rates;
	int num_current_rates = table->num_rates;

	if (num_current_rates != num_rates)
		return false;

	if (current_max_lane_count != old_max_lane_count)
		return false;

	if (memcmp(current_rates, rates, num_rates * sizeof(rates[0])))
		return false;

	return true;
}

/**
 * intel_dp_link_caps_update - rebuild the supported link configuration state
 * @link_caps: link capabilities state
 * @rates: supported common link rates
 * @num_rates: number of entries in @rates
 * @max_lane_count: supported maximum lane count
 *
 * Rebuild the supported link configuration state from @rates and
 * @max_lane_count.
 *
 * Configuration indices are not stable across calls to this function, so
 * callers should not cache such indices and masks built from them across
 * updates via this function.
 *
 * This function is called regularly, at least after a sink is connected,
 * but it may also be called later whenever the sink capabilities may have
 * changed, for example in response to HPD IRQ / RX_CAP_CHANGED signaling.
 *
 * In the Intel driver this function is currently called whenever the
 * connector detect handler runs, after reading the sink capabilities. This
 * may change if those capabilities are cached until the sink is
 * disconnected, or until RX_CAP_CHANGED is signaled. In any case, this
 * function should be called whenever the sink capabilities were read out
 * and may have changed.
 *
 * Returns:
 * - %true if the supported link parameters have changed, %false otherwise.
 */
bool intel_dp_link_caps_update(struct intel_dp_link_caps *link_caps,
			       const int *rates, int num_rates, int max_lane_count)
{
	struct intel_dp *intel_dp = link_caps->dp;
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_dp_link_caps_config_table *table =
		&link_caps->config_table;
	struct intel_dp_link_config old_max_limits =
		link_caps->max_limits;
	int old_rates[DP_MAX_SUPPORTED_RATES];
	struct intel_dp_link_config_entry *lc;
	bool link_params_changed = false;
	int num_common_lane_configs;
	int old_max_lane_count;
	int num_old_rates;
	int i;
	int j;

	if (drm_WARN_ON(display->drm, !is_power_of_2(max_lane_count)))
		return false;

	if (drm_WARN_ON(display->drm, num_rates > ARRAY_SIZE(table->rates)))
		return false;

	num_common_lane_configs = ilog2(max_lane_count) + 1;

	if (drm_WARN_ON(display->drm, num_rates * num_common_lane_configs >
				    ARRAY_SIZE(table->configs)))
		return false;

	num_old_rates = table->num_rates;
	memcpy(old_rates, table->rates, num_old_rates * sizeof(old_rates[0]));
	old_max_lane_count = table->max_lane_count;

	memcpy(table->rates, rates, num_rates * sizeof(rates[0]));
	table->num_rates = num_rates;
	table->max_lane_count = max_lane_count;

	table->num_configs = num_rates * num_common_lane_configs;

	lc = &table->configs[0];
	for (i = 0; i < num_rates; i++) {
		for (j = 0; j < num_common_lane_configs; j++) {
			lc->lane_count_exp = j;
			lc->link_rate_idx = i;

			lc++;
		}
	}

	sort_r(table->configs, table->num_configs,
	       sizeof(table->configs[0]),
	       link_config_cmp_by_bw, NULL,
	       table);

	if (!current_common_caps_match(table, old_rates, num_old_rates,
				       old_max_lane_count))
		link_params_changed = true;

	/*
	 * A failure could be only due to a bug, the update function handles
	 * that case by removing all restriction and resetting the max limit
	 * to the sink's maximum bounds.
	 */
	update_max_link_info(link_caps);

	if (link_caps->max_limits.rate != old_max_limits.rate)
		link_params_changed = true;

	if (link_caps->max_limits.lane_count != old_max_limits.lane_count)
		link_params_changed = true;

	return link_params_changed;
}

/**
 * intel_dp_link_caps_reset - reset link capability restrictions
 * @link_caps: link capabilities state
 *
 * Reset all current restrictions except for the user requested forced
 * parameters, thus updating the set of allowed configurations and the
 * derived maximum link information accordingly.
 *
 * This function is regularly called after a sink is connected, either
 * for the first time to the connector or after a previous sink was
 * disconnected from it, and intel_dp_link_caps_update() was called.
 */
void intel_dp_link_caps_reset(struct intel_dp_link_caps *link_caps)
{
	reset_max_link_limits_no_update(link_caps);
	/* On failure the following removes all restrictions. */
	update_max_link_info(link_caps);
}

static int i915_dp_force_link_rate_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	int current_rate = -1;
	int force_rate;
	int err;
	int i;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	if (intel_dp->link.active)
		current_rate = intel_dp->link_rate;

	force_rate = link_caps->forced_params.rate;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	seq_printf(m, "%sauto%s",
		   force_rate == 0 ? "[" : "",
		   force_rate == 0 ? "]" : "");

	for (i = 0; i < intel_dp->num_source_rates; i++)
		seq_printf(m, " %s%d%s%s",
			   intel_dp->source_rates[i] == force_rate ? "[" : "",
			   intel_dp->source_rates[i],
			   intel_dp->source_rates[i] == current_rate ? "*" : "",
			   intel_dp->source_rates[i] == force_rate ? "]" : "");

	seq_putc(m, '\n');

	return 0;
}

static int parse_link_rate(struct intel_dp_link_caps *link_caps, const char __user *ubuf, size_t len)
{
	struct intel_dp *intel_dp = link_caps->dp;
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
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	int rate;
	int err;

	rate = parse_link_rate(link_caps, ubuf, len);
	if (rate < 0)
		return rate;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	link_caps->forced_params.rate = rate;
	intel_dp_reset_link_params(intel_dp);

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	*offp += len;

	return len;
}
DEFINE_SHOW_STORE_ATTRIBUTE(i915_dp_force_link_rate);

static int i915_dp_force_lane_count_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = to_intel_connector(m->private);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	int current_lane_count = -1;
	int force_lane_count;
	int err;
	int i;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	if (intel_dp->link.active)
		current_lane_count = intel_dp->lane_count;
	force_lane_count = link_caps->forced_params.lane_count;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	seq_printf(m, "%sauto%s",
		   force_lane_count == 0 ? "[" : "",
		   force_lane_count == 0 ? "]" : "");

	for (i = 1; i <= 4; i <<= 1)
		seq_printf(m, " %s%d%s%s",
			   i == force_lane_count ? "[" : "",
			   i,
			   i == current_lane_count ? "*" : "",
			   i == force_lane_count ? "]" : "");

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
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	int lane_count;
	int err;

	lane_count = parse_lane_count(ubuf, len);
	if (lane_count < 0)
		return lane_count;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	link_caps->forced_params.lane_count = lane_count;
	intel_dp_reset_link_params(intel_dp);

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	*offp += len;

	return len;
}
DEFINE_SHOW_STORE_ATTRIBUTE(i915_dp_force_lane_count);

static int i915_dp_max_link_rate_show(void *data, u64 *val)
{
	struct intel_connector *connector = to_intel_connector(data);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_config max_link_limits;
	int err;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	intel_dp_link_caps_get_max_limits(intel_dp->link.caps, &max_link_limits);
	*val = max_link_limits.rate;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(i915_dp_max_link_rate_fops, i915_dp_max_link_rate_show, NULL, "%llu\n");

static int i915_dp_max_lane_count_show(void *data, u64 *val)
{
	struct intel_connector *connector = to_intel_connector(data);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_config max_link_limits;
	int err;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	intel_dp_link_caps_get_max_limits(intel_dp->link.caps, &max_link_limits);
	*val = max_link_limits.lane_count;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(i915_dp_max_lane_count_fops, i915_dp_max_lane_count_show, NULL, "%llu\n");

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
}

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
