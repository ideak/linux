// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i915_drv.h"

#include "intel_atomic.h"
#include "intel_display_types.h"
#include "intel_dp_mst.h"
#include "intel_fdi.h"
#include "intel_link_bw.h"

/**
 * intel_link_bw_init_limits - initialize BW limits
 * @i915: device instance
 * @limits: link BW limits
 *
 * Initialize @limits.
 */
void intel_link_bw_init_limits(struct drm_i915_private *i915, struct intel_link_bw_limits *limits)
{
	enum pipe pipe;

	limits->min_bpp_pipes = 0;
	for_each_pipe(i915, pipe)
		limits->max_bpp_x16[pipe] = INT_MAX;
}

/**
 * intel_link_bw_compute_pipe_bpp - compute pipe bpp limited by max link bpp
 * @crtc_state: the crtc state
 *
 * Compute the pipe bpp limited by the CRTC's maximum link bpp. Encoders can
 * call this function during state computation in the simple case where the
 * link bpp will always match the pipe bpp. This is the case for all non-DP
 * encoders, while DP encoders will use a link bpp lower than pipe bpp in case
 * of DSC compression.
 *
 * Returns %true in case of success, %false if pipe bpp would need to be
 * reduced below its valid range.
 */
bool intel_link_bw_compute_pipe_bpp(struct intel_crtc_state *crtc_state)
{
	int pipe_bpp = min(crtc_state->pipe_bpp,
			   to_bpp_int(crtc_state->max_link_bpp_x16));

	pipe_bpp = rounddown(pipe_bpp, 2 * 3);

	if (pipe_bpp < 6 * 3)
		return false;

	crtc_state->pipe_bpp = pipe_bpp;

	return true;
}

/**
 * intel_link_bw_reduce_bpp - reduce maximum link bpp for a selected pipe
 * @state: atomic state
 * @limits: link BW limits
 * @pipe_mask: mask of pipes to select from
 * @reason: explanation of why bpp reduction is needed
 *
 * Select the pipe from @pipe_mask with the biggest link bpp value and set the
 * maximum of link bpp in @limits below this value. Modeset the selected pipe,
 * so that its state will get recomputed.
 *
 * This function can be called to resolve a link's BW overallocation by reducing
 * the link bpp of one pipe on the link and hence reducing the total link BW.
 *
 * Returns
 *   - 0 in case of success
 *   - %-ENOSPC if no pipe can further reduce its link bpp
 *   - Other negative error, if modesetting the selected pipe failed
 */
int intel_link_bw_reduce_bpp(struct intel_atomic_state *state,
			     struct intel_link_bw_limits *limits,
			     u8 pipe_mask,
			     const char *reason)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	enum pipe max_bpp_pipe = INVALID_PIPE;
	struct intel_crtc *crtc;
	int max_bpp = 0;

	for_each_intel_crtc_in_pipe_mask(&i915->drm, crtc, pipe_mask) {
		struct intel_crtc_state *crtc_state;
		int pipe_bpp;

		if (limits->min_bpp_pipes & BIT(crtc->pipe))
			continue;

		crtc_state = intel_atomic_get_crtc_state(&state->base,
							 crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		if (crtc_state->dsc.compression_enable)
			pipe_bpp = crtc_state->dsc.compressed_bpp;
		else
			pipe_bpp = crtc_state->pipe_bpp;

		if (pipe_bpp > max_bpp) {
			max_bpp = pipe_bpp;
			max_bpp_pipe = crtc->pipe;
		}
	}

	if (max_bpp_pipe == INVALID_PIPE)
		return -ENOSPC;

	limits->max_bpp_x16[max_bpp_pipe] = to_bpp_x16(max_bpp) - 1;

	return intel_modeset_pipes_in_mask_early(state, reason,
						 BIT(max_bpp_pipe));
}

/**
 * intel_link_bw_reset_pipe_limit_to_min - reset BW limit for a pipe to its minimum
 * @state: atomic state
 * @old_limits: link BW limits
 * @new_limits: link BW limits
 * @pipe: pipe
 *
 * Reset the link bpp limit for @pipe in @new_limits to its value in
 * @old_limits and mark this limit as the minimum. This function must be
 * called after a pipe's compute config function failed, @old_limits
 * containing the bpp limit with which compute config previously passed.
 *
 * The function will fail if setting a minimum is not possible, either
 * because the old and new limits match (and so would lead to a pipe compute
 * config failure) or the limit is already at the minimum.
 *
 * Returns %true in case of success.
 */
bool
intel_link_bw_reset_pipe_limit_to_min(struct intel_atomic_state *state,
				       const struct intel_link_bw_limits *old_limits,
				       struct intel_link_bw_limits *new_limits,
				       enum pipe pipe)
{
	if (pipe == INVALID_PIPE)
		return false;

	if (new_limits->min_bpp_pipes & BIT(pipe))
		return false;

	if (new_limits->max_bpp_x16[pipe] ==
	    old_limits->max_bpp_x16[pipe])
		return false;

	new_limits->max_bpp_x16[pipe] =
		old_limits->max_bpp_x16[pipe];
	new_limits->min_bpp_pipes |= BIT(pipe);

	return true;
}

static int check_all_link_config(struct intel_atomic_state *state,
				 struct intel_link_bw_limits *limits)
{
	/* TODO: Check all shared display link configurations like FDI */
	return 0;
}

static bool
assert_link_limit_change_valid(struct drm_i915_private *i915,
			       const struct intel_link_bw_limits *old_limits,
			       const struct intel_link_bw_limits *new_limits)
{
	bool bpps_changed = false;
	enum pipe pipe;

	for_each_pipe(i915, pipe) {
		/* The bpp limit can only decrease. */
		if (drm_WARN_ON(&i915->drm,
				new_limits->max_bpp_x16[pipe] >
				old_limits->max_bpp_x16[pipe]))
			return false;

		if (new_limits->max_bpp_x16[pipe] <
		    old_limits->max_bpp_x16[pipe])
			bpps_changed = true;
	}

	/* At least one limit must change. */
	if (drm_WARN_ON(&i915->drm,
			!bpps_changed))
		return false;

	return true;
}

/**
 * intel_link_bw_atomic_check - check display link states and set a fallback config if needed
 * @state: atomic state
 * @new_limits: link BW limits
 *
 * Check the configuration of all shared display links in @state and set new BW
 * limits in @new_limits if there is a BW limitation.
 *
 * Returns:
 *   - 0 if the confugration is valid
 *   - %-EAGAIN, if the configuration is invalid and @new_limits got updated
 *     with fallback values with which the configuration of all CRTCs
 *     in @state must be recomputed
 *   - Other negative error, if the configuration is invalid without a
 *     fallback possibility, or the check failed for another reason
 */
int intel_link_bw_atomic_check(struct intel_atomic_state *state,
			       struct intel_link_bw_limits *new_limits)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_link_bw_limits old_limits = *new_limits;
	int ret;

	ret = check_all_link_config(state, new_limits);
	if (ret != -EAGAIN)
		return ret;

	if (!assert_link_limit_change_valid(i915, &old_limits, new_limits))
		return -EINVAL;

	return -EAGAIN;
}
