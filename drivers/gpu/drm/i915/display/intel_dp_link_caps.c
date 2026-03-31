// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/types.h>

#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_caps.h"

struct intel_dp_link_caps {
	struct intel_dp *dp;

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

	/*
	 * Forced parameters requested via debugfs. Remains set across sink
	 * disconnects.
	 */
	struct intel_dp_link_config forced_params;
};

/* Get length of common rates array potentially limited by max_rate. */
int intel_dp_common_len_rate_limit(const struct intel_dp *intel_dp,
				   int max_rate)
{
	return intel_dp_rate_limit_len(intel_dp->common_rates,
				       intel_dp->num_common_rates, max_rate);
}

int intel_dp_common_rate(struct intel_dp *intel_dp, int index)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (drm_WARN_ON(display->drm,
			index < 0 || index >= intel_dp->num_common_rates))
		return 162000;

	return intel_dp->common_rates[index];
}

/* Theoretical max between source and sink */
int intel_dp_max_common_rate(struct intel_dp *intel_dp)
{
	return intel_dp_common_rate(intel_dp, intel_dp->num_common_rates - 1);
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
	struct intel_dp *intel_dp = link_caps->dp;

	*rates = intel_dp->common_rates;
	*num_rates = intel_dp->num_common_rates;
}

static int forced_lane_count(struct intel_dp *intel_dp)
{
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;

	if (!link_caps->forced_params.lane_count)
		return 0;

	return clamp(link_caps->forced_params.lane_count,
		     1, intel_dp_max_common_lane_count(intel_dp));
}

static int forced_link_rate(struct intel_dp *intel_dp)
{
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	int len;

	if (!link_caps->forced_params.rate)
		return 0;

	len = intel_dp_common_len_rate_limit(intel_dp, link_caps->forced_params.rate);
	if (len == 0)
		return intel_dp_common_rate(intel_dp, 0);

	return intel_dp_common_rate(intel_dp, len - 1);
}

void intel_dp_link_caps_get_forced_params(struct intel_dp_link_caps *link_caps,
					  struct intel_dp_link_config *forced_params)
{
	forced_params->rate = forced_link_rate(link_caps->dp);
	forced_params->lane_count = forced_lane_count(link_caps->dp);
}

static int intel_dp_link_config_rate(struct intel_dp *intel_dp,
				     const struct intel_dp_link_config_entry *lc)
{
	return intel_dp_common_rate(intel_dp, lc->link_rate_idx);
}

static int intel_dp_link_config_lane_count(const struct intel_dp_link_config_entry *lc)
{
	return 1 << lc->lane_count_exp;
}

static int intel_dp_link_config_bw(struct intel_dp *intel_dp,
				   const struct intel_dp_link_config_entry *lc)
{
	return drm_dp_max_dprx_data_rate(intel_dp_link_config_rate(intel_dp, lc),
					 intel_dp_link_config_lane_count(lc));
}

static int link_config_cmp_by_bw(const void *a, const void *b, const void *p)
{
	struct intel_dp *intel_dp = (struct intel_dp *)p;	/* remove const */
	const struct intel_dp_link_config_entry *lc_a = a;
	const struct intel_dp_link_config_entry *lc_b = b;
	int bw_a = intel_dp_link_config_bw(intel_dp, lc_a);
	int bw_b = intel_dp_link_config_bw(intel_dp, lc_b);

	if (bw_a != bw_b)
		return bw_a - bw_b;

	return intel_dp_link_config_rate(intel_dp, lc_a) -
	       intel_dp_link_config_rate(intel_dp, lc_b);
}

static bool current_common_caps_match(struct intel_dp *intel_dp,
				      const int *rates, int num_rates)
{
	const int *current_rates = intel_dp->common_rates;
	int num_current_rates = intel_dp->num_common_rates;

	if (num_current_rates != num_rates)
		return false;

	if (memcmp(current_rates, rates, num_rates * sizeof(rates[0])))
		return false;

	return true;
}

/* Return %true if the supported link parameters have changed. */
bool intel_dp_link_caps_update(struct intel_dp *intel_dp,
			       const int *rates, int num_rates)
{
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	struct intel_display *display = to_intel_display(intel_dp);
	int old_rates[DP_MAX_SUPPORTED_RATES];
	struct intel_dp_link_config_entry *lc;
	bool link_params_changed = false;
	int num_common_lane_configs;
	int num_old_rates;
	int i;
	int j;

	if (drm_WARN_ON(display->drm, !is_power_of_2(intel_dp_max_common_lane_count(intel_dp))))
		return false;

	if (drm_WARN_ON(display->drm, num_rates > ARRAY_SIZE(intel_dp->common_rates)))
		return false;

	num_common_lane_configs = ilog2(intel_dp_max_common_lane_count(intel_dp)) + 1;

	if (drm_WARN_ON(display->drm, num_rates * num_common_lane_configs >
				    ARRAY_SIZE(link_caps->configs)))
		return false;

	num_old_rates = intel_dp->num_common_rates;
	memcpy(old_rates, intel_dp->common_rates, num_old_rates * sizeof(old_rates[0]));

	memcpy(intel_dp->common_rates, rates, num_rates * sizeof(rates[0]));
	intel_dp->num_common_rates = num_rates;
	link_caps->num_configs = num_rates * num_common_lane_configs;

	lc = &link_caps->configs[0];
	for (i = 0; i < num_rates; i++) {
		for (j = 0; j < num_common_lane_configs; j++) {
			lc->lane_count_exp = j;
			lc->link_rate_idx = i;

			lc++;
		}
	}

	sort_r(link_caps->configs, link_caps->num_configs,
	       sizeof(link_caps->configs[0]),
	       link_config_cmp_by_bw, NULL,
	       intel_dp);

	if (!current_common_caps_match(intel_dp, old_rates, num_old_rates))
		link_params_changed = true;

	/* TODO: Also detect a change in the max lane count and max link limits. */
	return link_params_changed;
}

void intel_dp_link_config_get(struct intel_dp *intel_dp, int idx, int *link_rate, int *lane_count)
{
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	struct intel_display *display = to_intel_display(intel_dp);
	const struct intel_dp_link_config_entry *lc;

	if (drm_WARN_ON(display->drm, idx < 0 || idx >= link_caps->num_configs))
		idx = 0;

	lc = &link_caps->configs[idx];

	*link_rate = intel_dp_link_config_rate(intel_dp, lc);
	*lane_count = intel_dp_link_config_lane_count(lc);
}

int intel_dp_link_config_index(struct intel_dp *intel_dp, int link_rate, int lane_count)
{
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
	int link_rate_idx = intel_dp_rate_index(intel_dp->common_rates, intel_dp->num_common_rates,
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
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_dp_link_caps *link_caps = intel_dp->link.caps;
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
	link_caps->forced_params.rate = rate;

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

	intel_dp_reset_link_params(intel_dp);
	link_caps->forced_params.lane_count = lane_count;

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
	int err;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	*val = intel_dp->link.max_rate;

	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(i915_dp_max_link_rate_fops, i915_dp_max_link_rate_show, NULL, "%llu\n");

static int i915_dp_max_lane_count_show(void *data, u64 *val)
{
	struct intel_connector *connector = to_intel_connector(data);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	int err;

	err = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (err)
		return err;

	intel_dp_flush_connector_commits(connector);

	*val = intel_dp->link.max_lane_count;

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
