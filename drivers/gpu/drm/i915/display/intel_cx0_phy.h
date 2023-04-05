// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_CX0_PHY_H__
#define __INTEL_CX0_PHY_H__

#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/bits.h>

#include "i915_drv.h"
#include "intel_display_types.h"

struct drm_i915_private;
struct intel_encoder;
struct intel_crtc_state;
enum phy;

#define for_each_cx0_lane_in_mask(__lane_mask, __lane) \
        for ((__lane) = 0; (__lane) < 2; (__lane)++) \
                for_each_if((__lane_mask) & BIT(__lane))

#define INTEL_CX0_LANE0		BIT(0)
#define INTEL_CX0_LANE1		BIT(1)
#define INTEL_CX0_BOTH_LANES	(INTEL_CX0_LANE1 | INTEL_CX0_LANE0)

bool intel_is_c10phy(struct drm_i915_private *dev_priv, enum phy phy);
void intel_cx0pll_enable(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state);
void intel_cx0pll_disable(struct intel_encoder *encoder);
void intel_c10mpllb_readout_hw_state(struct intel_encoder *encoder,
				     struct intel_c10mpllb_state *pll_state);
int intel_cx0mpllb_calc_state(struct intel_crtc_state *crtc_state,
			      struct intel_encoder *encoder);
void intel_c10mpllb_dump_hw_state(struct drm_i915_private *dev_priv,
				  const struct intel_c10mpllb_state *hw_state);
int intel_c10mpllb_calc_port_clock(struct intel_encoder *encoder,
				   const struct intel_c10mpllb_state *pll_state);
void intel_c10mpllb_state_verify(struct intel_atomic_state *state,
				 struct intel_crtc_state *new_crtc_state);

#endif /* __INTEL_CX0_PHY_H__ */
