#include "i915_drv.h"

#include "intel_atomic.h"
#include "intel_display_types.h"
#include "intel_dp_mst.h"
#include "intel_fdi.h"
#include "intel_link_bw.h"

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
 * intel_link_bw_reduce_link_bpp - reduce maximum link bpp for a selected pipe
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
int intel_link_bw_reduce_link_bpp(struct intel_atomic_state *state,
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

	return intel_modeset_pipes_in_mask(state, reason,
					   BIT(max_bpp_pipe), false);
}

bool
intel_link_bw_reset_pipe_limit_to_min(struct intel_atomic_state *state,
				      const struct intel_link_bw_limits *old_limits,
				      struct intel_link_bw_limits *new_limits,
				      enum pipe failed_pipe)
{
	if (failed_pipe == INVALID_PIPE)
		return false;

	if (new_limits->min_bpp_pipes & BIT(failed_pipe))
		return false;

	if (new_limits->max_bpp_x16[failed_pipe] ==
	    old_limits->max_bpp_x16[failed_pipe])
		return false;

	new_limits->max_bpp_x16[failed_pipe] =
		old_limits->max_bpp_x16[failed_pipe];
	new_limits->min_bpp_pipes |= BIT(failed_pipe);

	return true;
}


static bool
assert_link_limit_change_valid(struct drm_i915_private *i915,
			       const struct intel_link_bw_limits *old_limits,
			       const struct intel_link_bw_limits *new_limits)
{
	bool bpps_changed = false;
	enum pipe pipe;

	/* FEC can't be forced off after it was forced on. */
	if (drm_WARN_ON(&i915->drm,
			(old_limits->force_fec_pipes & new_limits->force_fec_pipes) !=
			old_limits->force_fec_pipes))
		return false;

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
			!bpps_changed &&
			new_limits->force_fec_pipes ==
			old_limits->force_fec_pipes))
		return false;

	return true;
}


int intel_link_bw_atomic_check(struct intel_atomic_state *state,
			       const struct intel_link_bw_limits *old_limits,
			       struct intel_link_bw_limits *new_limits)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	int ret;

	ret = intel_fdi_atomic_check_link(state, new_limits);
	if (ret)
		return ret;

	ret = intel_dp_mst_atomic_check_link(state, new_limits);
	if (ret)
		return ret;

	if (!assert_link_limit_change_valid(i915,
					    old_limits,
					    new_limits))
		return -EINVAL;

	return 0;
}

