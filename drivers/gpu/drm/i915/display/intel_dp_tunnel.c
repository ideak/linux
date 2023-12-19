// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i915_drv.h"

#include <drm/display/drm_dp_tunnel.h>

#include "intel_atomic.h"
#include "intel_display_limits.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_link_training.h"
#include "intel_dp_mst.h"
#include "intel_dp_tunnel.h"
#include "intel_link_bw.h"

struct intel_dp_tunnel_inherited_state {
	struct {
		struct drm_dp_tunnel_ref tunnel_ref;
	} tunnels[I915_MAX_PIPES];
};

static void destroy_tunnel(struct intel_dp *intel_dp)
{
	drm_dp_tunnel_destroy(intel_dp->tunnel);
	intel_dp->tunnel = NULL;
}

void intel_dp_tunnel_disconnect(struct intel_dp *intel_dp)
{
	if (!intel_dp->tunnel)
		return;

	destroy_tunnel(intel_dp);
}

void intel_dp_tunnel_destroy(struct intel_dp *intel_dp)
{
	if (!intel_dp->tunnel)
		return;

	if (intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		drm_dp_tunnel_disable_bw_alloc(intel_dp->tunnel);

	destroy_tunnel(intel_dp);
}

static int kbytes_to_mbits(int kbytes)
{
	return DIV_ROUND_UP(kbytes * 8, 1000);
}

static int get_current_link_bw(struct intel_dp *intel_dp,
			       bool *below_dprx_bw)
{
	int rate = intel_dp_max_common_rate(intel_dp);
	int lane_count = intel_dp_max_common_lane_count(intel_dp);
	int bw;

	bw = intel_dp_max_link_data_rate(intel_dp, rate, lane_count);
	*below_dprx_bw = bw < drm_dp_max_dprx_data_rate(rate, lane_count);

	return bw;
}

static int update_tunnel_state(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	bool old_bw_below_dprx;
	bool new_bw_below_dprx;
	int old_bw;
	int new_bw;
	int ret;

	old_bw = get_current_link_bw(intel_dp, &old_bw_below_dprx);

	ret = drm_dp_tunnel_update_state(intel_dp->tunnel);
	if (ret < 0) {
		drm_dbg_kms(&i915->drm,
			    "[DPTUN %s][ENCODER:%d:%s] State update failed (err %pe)\n",
			    drm_dp_tunnel_name(intel_dp->tunnel),
			    encoder->base.base.id,
			    encoder->base.name,
			    ERR_PTR(ret));

		return ret;
	}

	if (ret == 0 ||
	    !drm_dp_tunnel_bw_alloc_is_enabled(intel_dp->tunnel))
		return 0;

	intel_dp_update_sink_caps(intel_dp);

	new_bw = get_current_link_bw(intel_dp, &new_bw_below_dprx);

	/* Suppress the notification if the mode list can't change due to bw. */
	if (old_bw_below_dprx == new_bw_below_dprx &&
	    !new_bw_below_dprx)
		return 0;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][ENCODER:%d:%s] Notify users about BW change: %d -> %d\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    encoder->base.base.id,
		    encoder->base.name,
		    kbytes_to_mbits(old_bw),
		    kbytes_to_mbits(new_bw));

	return 1;
}

static int allocate_initial_tunnel_bw(struct intel_dp *intel_dp,
				      struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	const struct intel_crtc *crtc;
	int tunnel_bw = 0;
	u8 pipe_mask;
	int err;

	err = intel_dp_get_active_pipes(intel_dp, ctx,
					INTEL_DP_GET_PIPES_SYNC,
					&pipe_mask);
	if (err)
		return err;

	for_each_intel_crtc_in_pipe_mask(&i915->drm, crtc, pipe_mask) {
		const struct intel_crtc_state *crtc_state =
			to_intel_crtc_state(crtc->base.state);
		int stream_bw = intel_dp_config_required_rate(crtc_state);

		drm_dbg_kms(&i915->drm,
			    "[DPTUN %s][ENCODER:%d:%s][CRTC:%d:%s] Initial BW for stream %d: %d/%d Mb/s\n",
			    drm_dp_tunnel_name(intel_dp->tunnel),
			    encoder->base.base.id,
			    encoder->base.name,
			    crtc->base.base.id,
			    crtc->base.name,
			    crtc->pipe,
			    kbytes_to_mbits(stream_bw),
			    kbytes_to_mbits(tunnel_bw));

		tunnel_bw += stream_bw;
	}

	err = drm_dp_tunnel_alloc_bw(intel_dp->tunnel, tunnel_bw);
	if (err) {
		drm_dbg_kms(&i915->drm,
			    "[DPTUN %s][ENCODER:%d:%s] Initial BW allocation failed (err %pe)\n",
			    drm_dp_tunnel_name(intel_dp->tunnel),
			    encoder->base.base.id,
			    encoder->base.name,
			    ERR_PTR(err));

		return err;
	}

	return update_tunnel_state(intel_dp);
}

static int detect_new_tunnel(struct intel_dp *intel_dp, struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	struct drm_dp_tunnel *tunnel;
	int ret;

	tunnel = drm_dp_tunnel_detect(i915->display.dp_tunnel_mgr,
					&intel_dp->aux);
	if (IS_ERR(tunnel))
		return PTR_ERR(tunnel);

	intel_dp->tunnel = tunnel;

	ret = drm_dp_tunnel_enable_bw_alloc(intel_dp->tunnel);
	if (ret) {
		if (ret == -EOPNOTSUPP)
			return 0;

		drm_dbg_kms(&i915->drm,
			    "[DPTUN %s][ENCODER:%d:%s] Failed to enable BW allocation mode (ret %pe)\n",
			    drm_dp_tunnel_name(intel_dp->tunnel),
			    encoder->base.base.id,
			    encoder->base.name,
			    ERR_PTR(ret));

		/* Keep the tunnel with BWA disabled */
		return 0;
	}

	ret = allocate_initial_tunnel_bw(intel_dp, ctx);
	if (ret < 0)
		intel_dp_tunnel_destroy(intel_dp);

	return ret;
}

int intel_dp_tunnel_detect(struct intel_dp *intel_dp, struct drm_modeset_acquire_ctx *ctx)
{
	int ret;

	if (intel_dp_is_edp(intel_dp))
		return 0;

	if (intel_dp->tunnel) {
		ret = update_tunnel_state(intel_dp);
		if (ret >= 0)
			return ret;

		/* Try to recreate the tunnel after an update error. */
		intel_dp_tunnel_destroy(intel_dp);
	}

	ret = detect_new_tunnel(intel_dp, ctx);
	if (ret >= 0 || ret == -EDEADLK)
		return ret;

	return ret;
}

bool intel_dp_tunnel_bw_alloc_is_enabled(struct intel_dp *intel_dp)
{
	return intel_dp->tunnel &&
		drm_dp_tunnel_bw_alloc_is_enabled(intel_dp->tunnel);
}

void intel_dp_tunnel_suspend(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;

	if (!intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		return;

	drm_dbg_kms(&i915->drm, "[DPTUN %s][CONNECTOR:%d:%s][ENCODER:%d:%s] Suspend\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    connector->base.base.id, connector->base.name,
		    encoder->base.base.id, encoder->base.name);

	intel_dp->tunnel_suspended = true;
}

void intel_dp_tunnel_resume(struct intel_dp *intel_dp, bool dpcd_updated)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	int err = 0;

	if (!intel_dp->tunnel_suspended)
		return;

	intel_dp->tunnel_suspended = false;

	drm_dbg_kms(&i915->drm, "[DPTUN %s][CONNECTOR:%d:%s][ENCODER:%d:%s] Resume\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    connector->base.base.id, connector->base.name,
		    encoder->base.base.id, encoder->base.name);

	/* DPRX caps read required by tunnel detection */
	if (!dpcd_updated)
		err = intel_dp_read_dprx_caps(intel_dp, dpcd);

	if (err)
		drm_dp_tunnel_set_io_error(intel_dp->tunnel);
	else
		err = drm_dp_tunnel_enable_bw_alloc(intel_dp->tunnel);
		/* TODO: allocate initial BW */

	if (!err)
		return;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][CONNECTOR:%d:%s][ENCODER:%d:%s] Tunnel can't be resumed, will drop and redect it (err %pe)\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    connector->base.base.id, connector->base.name,
		    encoder->base.base.id, encoder->base.name,
		    ERR_PTR(err));
}

static struct drm_dp_tunnel *
get_inherited_tunnel_state(struct intel_atomic_state *state,
			   const struct intel_crtc *crtc)
{
	if (!state->dp_tunnel_state)
		return NULL;

	return state->dp_tunnel_state->tunnels[crtc->pipe].tunnel_ref.tunnel;
}

static int
add_inherited_tunnel_state(struct intel_atomic_state *state,
			   struct drm_dp_tunnel *tunnel,
			   const struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct drm_dp_tunnel *old_tunnel;

	old_tunnel = get_inherited_tunnel_state(state, crtc);
	if (old_tunnel) {
		drm_WARN_ON(&i915->drm, old_tunnel != tunnel);
		return 0;
	}

	if (!state->dp_tunnel_state) {
		state->dp_tunnel_state = kzalloc(sizeof(*state->dp_tunnel_state), GFP_KERNEL);
		if (!state->dp_tunnel_state)
			return -ENOMEM;
	}

	drm_dp_tunnel_ref_get(tunnel,
			      &state->dp_tunnel_state->tunnels[crtc->pipe].tunnel_ref);

	return 0;
}

static int check_inherited_tunnel_state(struct intel_atomic_state *state,
					struct intel_dp *intel_dp,
					const struct intel_digital_connector_state *old_conn_state)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	const struct intel_connector *connector =
		to_intel_connector(old_conn_state->base.connector);
	struct intel_crtc *old_crtc;
	const struct intel_crtc_state *old_crtc_state;

	/*
	 * If a BWA tunnel gets detected only after the corresponding
	 * connector got enabled already without a BWA tunnel, or a different
	 * BWA tunnel (which was removed meanwhile) the old CRTC state won't
	 * contain the state of the current tunnel. This tunnel still has a
	 * reserved BW, which needs to be released, add the state for such
	 * inherited tunnels separately only to this atomic state.
	 */
	if (!intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		return 0;

	if (!old_conn_state->base.crtc)
		return 0;

	old_crtc = to_intel_crtc(old_conn_state->base.crtc);
	old_crtc_state = intel_atomic_get_old_crtc_state(state, old_crtc);

	if (!old_crtc_state->hw.active ||
	    old_crtc_state->dp_tunnel_ref.tunnel == intel_dp->tunnel)
		return 0;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][CONNECTOR:%d:%s][ENCODER:%d:%s][CRTC:%d:%s] Adding state for inherited tunnel %p\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    connector->base.base.id,
		    connector->base.name,
		    encoder->base.base.id,
		    encoder->base.name,
		    old_crtc->base.base.id,
		    old_crtc->base.name,
		    intel_dp->tunnel);

	return add_inherited_tunnel_state(state, intel_dp->tunnel, old_crtc);
}

void intel_dp_tunnel_atomic_cleanup_inherited_state(struct intel_atomic_state *state)
{
	enum pipe pipe;

	if (!state->dp_tunnel_state)
		return;

	for_each_pipe(to_i915(state->base.dev), pipe)
		if (state->dp_tunnel_state->tunnels[pipe].tunnel_ref.tunnel)
			drm_dp_tunnel_ref_put(&state->dp_tunnel_state->tunnels[pipe].tunnel_ref);

	kfree(state->dp_tunnel_state);
	state->dp_tunnel_state = NULL;
}

static int intel_dp_tunnel_atomic_add_group_state(struct intel_atomic_state *state,
						  struct drm_dp_tunnel *tunnel)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	u32 pipe_mask;
	int err;

	if (!tunnel)
		return 0;

	err = drm_dp_tunnel_atomic_get_group_streams_in_state(&state->base,
							      tunnel, &pipe_mask);
	if (err)
		return err;

	drm_WARN_ON(&i915->drm, pipe_mask & ~((1 << I915_MAX_PIPES) - 1));

	return intel_modeset_pipes_in_mask_early(state, "DPTUN", pipe_mask);
}

int intel_dp_tunnel_atomic_add_state_for_crtc(struct intel_atomic_state *state,
					      struct intel_crtc *crtc)
{
	const struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct drm_dp_tunnel_state *tunnel_state;
	struct drm_dp_tunnel *tunnel = new_crtc_state->dp_tunnel_ref.tunnel;

	if (!tunnel)
		return 0;

	tunnel_state = drm_dp_tunnel_atomic_get_state(&state->base, tunnel);
	if (IS_ERR(tunnel_state))
		return PTR_ERR(tunnel_state);

	return 0;
}

static int check_group_state(struct intel_atomic_state *state,
			     struct intel_dp *intel_dp,
			     const struct intel_connector *connector,
			     struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	const struct intel_crtc_state *crtc_state;

	crtc_state = intel_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->dp_tunnel_ref.tunnel)
		return 0;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][CONNECTOR:%d:%s][ENCODER:%d:%s][CRTC:%d:%s] Adding group state for tunnel %p\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    connector->base.base.id,
		    connector->base.name,
		    encoder->base.base.id,
		    encoder->base.name,
		    crtc->base.base.id,
		    crtc->base.name,
		    intel_dp->tunnel);

	return intel_dp_tunnel_atomic_add_group_state(state, crtc_state->dp_tunnel_ref.tunnel);
}

int intel_dp_tunnel_atomic_check_state(struct intel_atomic_state *state,
				       struct intel_dp *intel_dp,
				       struct intel_connector *connector)
{
	const struct intel_digital_connector_state *old_conn_state =
		intel_atomic_get_new_connector_state(state, connector);
	const struct intel_digital_connector_state *new_conn_state =
		intel_atomic_get_new_connector_state(state, connector);
	int err;

	if (old_conn_state->base.crtc) {
		err = check_group_state(state, intel_dp, connector,
					to_intel_crtc(old_conn_state->base.crtc));
		if (err)
			return err;
	}

	if (new_conn_state->base.crtc &&
	    new_conn_state->base.crtc != old_conn_state->base.crtc) {
		err = check_group_state(state, intel_dp, connector,
					to_intel_crtc(new_conn_state->base.crtc));
		if (err)
			return err;
	}

	return check_inherited_tunnel_state(state, intel_dp, old_conn_state);
}

void intel_dp_tunnel_atomic_compute_stream_bw(struct intel_atomic_state *state,
					      struct intel_dp *intel_dp,
					      const struct intel_connector *connector,
					      struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	const struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	int required_rate = intel_dp_config_required_rate(crtc_state);

	if (!intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		return;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][CONNECTOR:%d:%s][ENCODER:%d:%s][CRTC:%d:%s] Stream %d required BW %d Mb/s\n",
		    drm_dp_tunnel_name(intel_dp->tunnel),
		    connector->base.base.id,
		    connector->base.name,
		    encoder->base.base.id,
		    encoder->base.name,
		    crtc->base.base.id,
		    crtc->base.name,
		    crtc->pipe,
		    kbytes_to_mbits(required_rate));

	drm_dp_tunnel_atomic_set_stream_bw(&state->base, intel_dp->tunnel,
					   crtc->pipe, required_rate);

	drm_dp_tunnel_ref_get(intel_dp->tunnel,
			      &crtc_state->dp_tunnel_ref);
}

/**
 * intel_dp_tunnel_atomic_check_link - Check the DP tunnel atomic state
 * @state: intel atomic state
 * @limits: link BW limits
 *
 * Check the link configuration for all DP tunnels in @state. If the
 * configuration is invalid @limits will be updated if possible to
 * reduce the total BW, after which the configuration for all CRTCs in
 * @state must be recomputed with the updated @limits.
 *
 * Returns:
 *   - 0 if the confugration is valid
 *   - %-EAGAIN, if the configuration is invalid and @limits got updated
 *     with fallback values with which the configuration of all CRTCs in
 *     @state must be recomputed
 *   - Other negative error, if the configuration is invalid without a
 *     fallback possibility, or the check failed for another reason
 */
int intel_dp_tunnel_atomic_check_link(struct intel_atomic_state *state,
				      struct intel_link_bw_limits *limits)
{
	u32 failed_stream_mask;
	int err;

	err = drm_dp_tunnel_atomic_check_stream_bws(&state->base,
						    &failed_stream_mask);
	if (err != -ENOSPC)
		return err;

	err = intel_link_bw_reduce_bpp(state, limits,
				       failed_stream_mask, "DP tunnel link BW");

	return err ? : -EAGAIN;
}

void intel_dp_tunnel_atomic_alloc_bw(struct intel_atomic_state *state,
				     struct intel_encoder *encoder,
				     const struct intel_crtc_state *new_crtc_state,
				     const struct drm_connector_state *new_conn_state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct drm_dp_tunnel *tunnel = new_crtc_state->dp_tunnel_ref.tunnel;
	const struct drm_dp_tunnel_state *new_tunnel_state;
	int err;

	if (!tunnel)
		return;

	new_tunnel_state = drm_dp_tunnel_atomic_get_new_state(&state->base, tunnel);

	err = drm_dp_tunnel_alloc_bw(tunnel,
				     drm_dp_tunnel_atomic_get_tunnel_bw(new_tunnel_state));
	if (!err)
		return;

	if (!intel_digital_port_connected(encoder))
		return;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][ENCODER:%d:%s] BW allocation failed on a connected sink (err %pe)\n",
		    drm_dp_tunnel_name(tunnel),
		    encoder->base.base.id,
		    encoder->base.name,
		    ERR_PTR(err));

	intel_dp_queue_modeset_retry_for_link(state, encoder, new_crtc_state, new_conn_state);
}

void intel_dp_tunnel_atomic_free_bw(struct intel_atomic_state *state,
				    struct intel_encoder *encoder,
				    const struct intel_crtc_state *old_crtc_state,
				    const struct drm_connector_state *old_conn_state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_crtc *old_crtc = to_intel_crtc(old_crtc_state->uapi.crtc);
	struct drm_dp_tunnel *tunnel;
	int err;

	tunnel = get_inherited_tunnel_state(state, old_crtc);
	if (!tunnel)
		tunnel = old_crtc_state->dp_tunnel_ref.tunnel;

	if (!tunnel)
		return;

	err = drm_dp_tunnel_alloc_bw(tunnel, 0);
	if (!err)
		return;

	if (!intel_digital_port_connected(encoder))
		return;

	drm_dbg_kms(&i915->drm,
		    "[DPTUN %s][ENCODER:%d:%s] BW freeing failed on a connected sink (err %pe)\n",
		    drm_dp_tunnel_name(tunnel),
		    encoder->base.base.id,
		    encoder->base.name,
		    ERR_PTR(err));

	intel_dp_queue_modeset_retry_for_link(state, encoder, old_crtc_state, old_conn_state);
}

int intel_dp_tunnel_mgr_init(struct drm_i915_private *i915)
{
	struct drm_dp_tunnel_mgr *tunnel_mgr;
	struct drm_connector_list_iter connector_list_iter;
	struct intel_connector *connector;
	int dp_connectors = 0;

	drm_connector_list_iter_begin(&i915->drm, &connector_list_iter);
	for_each_intel_connector_iter(connector, &connector_list_iter) {
		if (connector->base.connector_type != DRM_MODE_CONNECTOR_DisplayPort)
			continue;

		dp_connectors++;
	}
	drm_connector_list_iter_end(&connector_list_iter);

	tunnel_mgr = drm_dp_tunnel_mgr_create(&i915->drm, dp_connectors);
	if (IS_ERR(tunnel_mgr))
		return PTR_ERR(tunnel_mgr);

	i915->display.dp_tunnel_mgr = tunnel_mgr;

	return 0;
}

void intel_dp_tunnel_mgr_cleanup(struct drm_i915_private *i915)
{
	drm_dp_tunnel_mgr_destroy(i915->display.dp_tunnel_mgr);
	i915->display.dp_tunnel_mgr = NULL;
}
