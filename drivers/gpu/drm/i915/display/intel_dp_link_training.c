/*
 * Copyright © 2008-2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_training.h"

static void
intel_dp_dump_link_status(const u8 link_status[DP_LINK_STATUS_SIZE])
{

	DRM_DEBUG_KMS("ln0_1:0x%x ln2_3:0x%x align:0x%x sink:0x%x adj_req0_1:0x%x adj_req2_3:0x%x",
		      link_status[0], link_status[1], link_status[2],
		      link_status[3], link_status[4], link_status[5]);
}

static u8 dp_voltage_max(u8 preemph)
{
	switch (preemph & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case DP_TRAIN_PRE_EMPH_LEVEL_0:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_3;
	case DP_TRAIN_PRE_EMPH_LEVEL_1:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_2;
	case DP_TRAIN_PRE_EMPH_LEVEL_2:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_1;
	case DP_TRAIN_PRE_EMPH_LEVEL_3:
	default:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_0;
	}
}

void intel_dp_get_adjust_train(struct intel_dp *intel_dp,
			       const u8 link_status[DP_LINK_STATUS_SIZE])
{
	u8 v = 0;
	u8 p = 0;
	int lane;
	u8 voltage_max;
	u8 preemph_max;

	for (lane = 0; lane < intel_dp->lane_count; lane++) {
		v = max(v, drm_dp_get_adjust_request_voltage(link_status, lane));
		p = max(p, drm_dp_get_adjust_request_pre_emphasis(link_status, lane));
	}

	preemph_max = intel_dp->preemph_max(intel_dp);
	if (p >= preemph_max)
		p = preemph_max | DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;

	voltage_max = min(intel_dp->voltage_max(intel_dp),
			  dp_voltage_max(p));
	if (v >= voltage_max)
		v = voltage_max | DP_TRAIN_MAX_SWING_REACHED;

	for (lane = 0; lane < 4; lane++)
		intel_dp->train_set[lane] = v | p;
}

static bool
intel_dp_set_link_train(struct intel_dp *intel_dp,
			u8 dp_train_pat)
{
	u8 buf[sizeof(intel_dp->train_set) + 1];
	int ret, len;

	intel_dp_program_link_training_pattern(intel_dp, dp_train_pat);

	buf[0] = dp_train_pat;
	if ((dp_train_pat & DP_TRAINING_PATTERN_MASK) ==
	    DP_TRAINING_PATTERN_DISABLE) {
		/* don't write DP_TRAINING_LANEx_SET on disable */
		len = 1;
	} else {
		/* DP_TRAINING_LANEx_SET follow DP_TRAINING_PATTERN_SET */
		memcpy(buf + 1, intel_dp->train_set, intel_dp->lane_count);
		len = intel_dp->lane_count + 1;
	}

	ret = drm_dp_dpcd_write(&intel_dp->aux,
				intel_dp->lttpr_set_offset + DP_TRAINING_PATTERN_SET,
				buf, len);

	return ret == len;
}

static bool
intel_dp_reset_link_train(struct intel_dp *intel_dp,
			u8 dp_train_pat)
{
	memset(intel_dp->train_set, 0, sizeof(intel_dp->train_set));
	intel_dp_set_signal_levels(intel_dp);
	return intel_dp_set_link_train(intel_dp, dp_train_pat);
}

static bool
intel_dp_update_link_train(struct intel_dp *intel_dp)
{
	int ret;

	intel_dp_set_signal_levels(intel_dp);

	ret = drm_dp_dpcd_write(&intel_dp->aux,
				intel_dp->lttpr_set_offset + DP_TRAINING_LANE0_SET,
				intel_dp->train_set, intel_dp->lane_count);

	return ret == intel_dp->lane_count;
}

static bool intel_dp_link_max_vswing_reached(struct intel_dp *intel_dp)
{
	int lane;

	for (lane = 0; lane < intel_dp->lane_count; lane++)
		if ((intel_dp->train_set[lane] &
		     DP_TRAIN_MAX_SWING_REACHED) == 0)
			return false;

	return true;
}

static void prepare_link_train(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	u8 link_config[2];
	u8 link_bw, rate_select;

	if (intel_dp->prepare_link_retrain)
		intel_dp->prepare_link_retrain(intel_dp);

	intel_dp_compute_rate(intel_dp, intel_dp->link_rate,
			      &link_bw, &rate_select);

	if (link_bw)
		drm_dbg_kms(&i915->drm,
			    "Using LINK_BW_SET value %02x\n", link_bw);
	else
		drm_dbg_kms(&i915->drm,
			    "Using LINK_RATE_SET value %02x\n", rate_select);

	/* Write the link configuration data */
	link_config[0] = link_bw;
	link_config[1] = intel_dp->lane_count;
	if (drm_dp_enhanced_frame_cap(intel_dp->dpcd))
		link_config[1] |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	drm_dp_dpcd_write(&intel_dp->aux, DP_LINK_BW_SET, link_config, 2);

	/* eDP 1.4 rate select method. */
	if (!link_bw)
		drm_dp_dpcd_write(&intel_dp->aux, DP_LINK_RATE_SET,
				  &rate_select, 1);

	link_config[0] = 0;
	link_config[1] = DP_SET_ANSI_8B10B;
	drm_dp_dpcd_write(&intel_dp->aux, DP_DOWNSPREAD_CTRL, link_config, 2);

	intel_dp->DP |= DP_PORT_EN;
}

/* Enable corresponding port and start training pattern 1 */
static bool
intel_dp_link_training_clock_recovery(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	u8 voltage;
	int voltage_tries, cr_tries, max_cr_tries;
	bool max_vswing_reached = false;

	/* clock recovery */
	if (!intel_dp_reset_link_train(intel_dp,
				       DP_TRAINING_PATTERN_1 |
				       DP_LINK_SCRAMBLING_DISABLE)) {
		drm_err(&i915->drm, "failed to enable link training\n");
		return false;
	}

	/*
	 * The DP 1.4 spec defines the max clock recovery retries value
	 * as 10 but for pre-DP 1.4 devices we set a very tolerant
	 * retry limit of 80 (4 voltage levels x 4 preemphasis levels x
	 * x 5 identical voltage retries). Since the previous specs didn't
	 * define a limit and created the possibility of an infinite loop
	 * we want to prevent any sync from triggering that corner case.
	 */
	if (intel_dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14)
		max_cr_tries = 10;
	else
		max_cr_tries = 80;

	voltage_tries = 1;
	for (cr_tries = 0; cr_tries < max_cr_tries; ++cr_tries) {
		u8 link_status[DP_LINK_STATUS_SIZE];

		drm_dp_link_train_clock_recovery_delay(intel_dp->dpcd);

		if (!intel_dp_get_link_status(intel_dp, link_status)) {
			drm_err(&i915->drm, "failed to get link status\n");
			return false;
		}

		if (drm_dp_clock_recovery_ok(link_status, intel_dp->lane_count)) {
			drm_dbg_kms(&i915->drm, "clock recovery OK\n");
			return true;
		}

		if (voltage_tries == 5) {
			drm_dbg_kms(&i915->drm,
				    "Same voltage tried 5 times\n");
			return false;
		}

		if (max_vswing_reached) {
			drm_dbg_kms(&i915->drm, "Max Voltage Swing reached\n");
			return false;
		}

		voltage = intel_dp->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK;

		/* Update training set as requested by target */
		intel_dp_get_adjust_train(intel_dp, link_status);
		if (!intel_dp_update_link_train(intel_dp)) {
			drm_err(&i915->drm,
				"failed to update link training\n");
			return false;
		}

		if ((intel_dp->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK) ==
		    voltage)
			++voltage_tries;
		else
			voltage_tries = 1;

		if (intel_dp_link_max_vswing_reached(intel_dp))
			max_vswing_reached = true;

	}
	drm_err(&i915->drm,
		"Failed clock recovery %d times, giving up!\n", max_cr_tries);
	return false;
}

/*
 * Pick training pattern for channel equalization. Training pattern 4 for HBR3
 * or for 1.4 devices that support it, training Pattern 3 for HBR2
 * or 1.2 devices that support it, Training Pattern 2 otherwise.
 */
static u32 intel_dp_training_pattern(struct intel_dp *intel_dp)
{
	bool source_tps3, sink_tps3, source_tps4, sink_tps4;

	/*
	 * Intel platforms that support HBR3 also support TPS4. It is mandatory
	 * for all downstream devices that support HBR3. There are no known eDP
	 * panels that support TPS4 as of Feb 2018 as per VESA eDP_v1.4b_E1
	 * specification.
	 */
	source_tps4 = intel_dp_source_supports_hbr3(intel_dp);
	sink_tps4 = drm_dp_tps4_supported(intel_dp->dpcd);
	if (source_tps4 && sink_tps4) {
		return DP_TRAINING_PATTERN_4;
	} else if (intel_dp->link_rate == 810000) {
		if (!source_tps4)
			drm_dbg_kms(&dp_to_i915(intel_dp)->drm,
				    "8.1 Gbps link rate without source HBR3/TPS4 support\n");
		if (!sink_tps4)
			drm_dbg_kms(&dp_to_i915(intel_dp)->drm,
				    "8.1 Gbps link rate without sink TPS4 support\n");
	}
	/*
	 * Intel platforms that support HBR2 also support TPS3. TPS3 support is
	 * also mandatory for downstream devices that support HBR2. However, not
	 * all sinks follow the spec.
	 */
	source_tps3 = intel_dp_source_supports_hbr2(intel_dp);
	sink_tps3 = drm_dp_tps3_supported(intel_dp->dpcd);
	if (source_tps3 && sink_tps3) {
		return  DP_TRAINING_PATTERN_3;
	} else if (intel_dp->link_rate >= 540000) {
		if (!source_tps3)
			drm_dbg_kms(&dp_to_i915(intel_dp)->drm,
				    ">=5.4/6.48 Gbps link rate without source HBR2/TPS3 support\n");
		if (!sink_tps3)
			drm_dbg_kms(&dp_to_i915(intel_dp)->drm,
				    ">=5.4/6.48 Gbps link rate without sink TPS3 support\n");
	}

	return DP_TRAINING_PATTERN_2;
}

static bool
intel_dp_link_training_channel_equalization(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	int tries;
	u32 training_pattern;
	u8 link_status[DP_LINK_STATUS_SIZE];
	bool channel_eq = false;

	training_pattern = intel_dp_training_pattern(intel_dp);
	/* Scrambling is disabled for TPS2/3 and enabled for TPS4 */
	if (training_pattern != DP_TRAINING_PATTERN_4)
		training_pattern |= DP_LINK_SCRAMBLING_DISABLE;

	/* channel equalization */
	if (!intel_dp_set_link_train(intel_dp,
				     training_pattern)) {
		drm_err(&i915->drm, "failed to start channel equalization\n");
		return false;
	}

	for (tries = 0; tries < 5; tries++) {

		drm_dp_link_train_channel_eq_delay(intel_dp->dpcd);
		if (!intel_dp_get_link_status(intel_dp, link_status)) {
			drm_err(&i915->drm,
				"failed to get link status\n");
			break;
		}

		/* Make sure clock is still ok */
		if (!drm_dp_clock_recovery_ok(link_status,
					      intel_dp->lane_count)) {
			intel_dp_dump_link_status(link_status);
			drm_dbg_kms(&i915->drm,
				    "Clock recovery check failed, cannot "
				    "continue channel equalization\n");
			break;
		}

		if (drm_dp_channel_eq_ok(link_status,
					 intel_dp->lane_count)) {
			channel_eq = true;
			drm_dbg_kms(&i915->drm, "Channel EQ done. DP Training "
				    "successful\n");
			break;
		}

		/* Update training set as requested by target */
		intel_dp_get_adjust_train(intel_dp, link_status);
		if (!intel_dp_update_link_train(intel_dp)) {
			drm_err(&i915->drm,
				"failed to update link training\n");
			break;
		}
	}

	/* Try 5 times, else fail and try at lower BW */
	if (tries == 5) {
		intel_dp_dump_link_status(link_status);
		drm_dbg_kms(&i915->drm,
			    "Channel equalization failed 5 times\n");
	}

	if (!intel_dp->lttpr_set_offset)
		intel_dp_set_idle_link_train(intel_dp);

	return channel_eq;

}

void intel_dp_stop_link_train(struct intel_dp *intel_dp)
{
	intel_dp->link_trained = true;

	intel_dp_set_link_train(intel_dp,
				DP_TRAINING_PATTERN_DISABLE);
}

static bool
__intel_dp_start_link_train(struct intel_dp *intel_dp)
{
	struct intel_connector *intel_connector = intel_dp->attached_connector;

	if (!intel_dp_link_training_clock_recovery(intel_dp))
		return false;

	if (!intel_dp_link_training_channel_equalization(intel_dp))
		return false;

	drm_dbg_kms(&dp_to_i915(intel_dp)->drm,
		    "[CONNECTOR:%d:%s] Link Training Passed at Link Rate = %d, Lane count = %d, at LTTPR %d",
		    intel_connector->base.base.id,
		    intel_connector->base.name,
		    intel_dp->link_rate, intel_dp->lane_count,
		    intel_dp->lttpr_instance);

	return true;
}

static void handle_link_train_fallback(struct intel_dp *intel_dp)
{
	struct intel_connector *intel_connector = intel_dp->attached_connector;

	drm_dbg_kms(&dp_to_i915(intel_dp)->drm,
		    "[CONNECTOR:%d:%s] Link Training failed at link rate = %d, lane count = %d, at LTTPR %d",
		    intel_connector->base.base.id,
		    intel_connector->base.name,
		    intel_dp->link_rate, intel_dp->lane_count,
		    intel_dp->lttpr_instance);
	if (!intel_dp_get_link_train_fallback_values(intel_dp,
						     intel_dp->link_rate,
						     intel_dp->lane_count))
		/* Schedule a Hotplug Uevent to userspace to start modeset */
		schedule_work(&intel_connector->modeset_retry_work);
	return;
}

static void init_lttpr(struct intel_dp *intel_dp, int idx)
{
	intel_dp->lttpr_instance = idx;

	if (idx) {
		intel_dp->lttpr_set_offset =
			DP_TRAINING_PATTERN_SET_PHY_REPEATER(idx) -
			DP_TRAINING_PATTERN_SET;
		intel_dp->lttpr_status_offset =
			DP_LANE0_1_STATUS_PHY_REPEATER(idx) -
			DP_LANE0_1_STATUS;
	} else {
		intel_dp->lttpr_set_offset = 0;
		intel_dp->lttpr_status_offset = 0;
	}
}

static bool train_link_with_lttpr_mode(struct intel_dp *intel_dp,
				       int phy_repeaters)
{
	bool ret = true;
	int i;

	prepare_link_train(intel_dp);

	for (i = phy_repeaters; i >= 0; i--) {
		u8 val;

		init_lttpr(intel_dp, i);

		ret = __intel_dp_start_link_train(intel_dp);
		if (!ret)
			break;

		val = DP_TRAINING_PATTERN_DISABLE;
		if (drm_dp_dpcd_write(&intel_dp->aux,
				      intel_dp->lttpr_set_offset + DP_TRAINING_PATTERN_SET,
				      &val, 1) != 1) {
			ret = false;
			break;
		}
	}

	init_lttpr(intel_dp, 0);

	return ret;
}

static int get_phy_repeater_count(u8 phy_repeater_count_code)
{
	switch (hweight8(phy_repeater_count_code)) {
	case 0:
		return 0;
	case 1:
		break;
	default:
		MISSING_CASE(phy_repeater_count_code);
		return 0;
	}

	return 8 - ilog2(phy_repeater_count_code);
}

static bool set_phy_repeater_mode(struct intel_dp *intel_dp, bool transparent)
{
	u8 val;
	int ret;

	val = DP_PHY_REPEATER_MODE_TRANSPARENT;
	ret = drm_dp_dpcd_write(&intel_dp->aux, DP_PHY_REPEATER_MODE, &val, 1);
	if (ret < 0)
		return false;

	if (transparent)
		return true;

	val = DP_PHY_REPEATER_MODE_NON_TRANSPARENT;
	ret = drm_dp_dpcd_write(&intel_dp->aux, DP_PHY_REPEATER_MODE, &val, 1);

	return ret >= 0;
}

static int check_phy_repeater_caps(struct intel_dp *intel_dp)
{
	u8 buf[5] = { };
	int lttprs;
	int max_link_rate;
	int max_link_lane_count;

	drm_dp_dpcd_read(&intel_dp->aux,
			 DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
			 buf, sizeof(buf));
	drm_dbg_kms(&dp_to_i915(intel_dp)->drm, "LTTPR info: %*ph\n",
		    (int)sizeof(buf), buf);

	lttprs = get_phy_repeater_count(buf[DP_PHY_REPEATER_CNT -
					DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV]);
	if (!lttprs)
		return 0;

	max_link_rate = drm_dp_bw_code_to_link_rate(buf[DP_MAX_LINK_RATE_PHY_REPEATER -
						        DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV]);
	WARN_ON(max_link_rate < intel_dp->max_link_rate);

	max_link_lane_count = buf[DP_MAX_LANE_COUNT_PHY_REPEATER -
				  DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV];

	WARN_ON(max_link_lane_count < intel_dp->max_link_lane_count);

	return lttprs;
}

void intel_dp_start_link_train(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	int phy_repeaters;
	bool ret;

	phy_repeaters = check_phy_repeater_caps(intel_dp);
	drm_dbg_kms(&i915->drm, "Number of LTTPRs: %d\n", phy_repeaters);

	if (phy_repeaters) {
		set_phy_repeater_mode(intel_dp, true);
		set_phy_repeater_mode(intel_dp, false);
	}

	ret = train_link_with_lttpr_mode(intel_dp, phy_repeaters);
	if (!ret && phy_repeaters) {
		drm_dbg_kms(&i915->drm,
			    "Link training in LTTPR non-transparent mode failed, retrying in transparent mode\n");
		ret = train_link_with_lttpr_mode(intel_dp, 0);
	}

	if (!ret)
		handle_link_train_fallback(intel_dp);
}
