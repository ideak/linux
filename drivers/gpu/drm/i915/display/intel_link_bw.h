#ifndef __INTEL_LINK_BW_H__
#define __INTEL_LINK_BW_H__

#include "intel_display_limits.h"

#include <linux/types.h>

struct intel_atomic_state;
struct intel_crtc_state;

struct intel_link_bw_limits {
	u8 force_fec_pipes;
	u8 min_bpp_pipes;
	/* in 1/16 bpp units */
	int max_bpp_x16[I915_MAX_PIPES];
};

bool intel_link_bw_compute_pipe_bpp(struct intel_crtc_state *crtc_state);
int intel_link_bw_reduce_link_bpp(struct intel_atomic_state *state,
				  struct intel_link_bw_limits *limits,
				  u8 pipe_mask,
				  const char *reason);
bool intel_link_bw_reset_pipe_limit_to_min(struct intel_atomic_state *state,
					   const struct intel_link_bw_limits *old_limits,
					   struct intel_link_bw_limits *new_limits,
					   enum pipe pipe);
int intel_link_bw_atomic_check(struct intel_atomic_state *state,
			       const struct intel_link_bw_limits *old_limits,
			       struct intel_link_bw_limits *new_limits);

#endif
