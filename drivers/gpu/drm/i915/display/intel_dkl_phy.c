// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "intel_de.h"
#include "intel_display.h"
#include "intel_dkl_phy.h"
#include "intel_dkl_phy_regs.h"

/*
 * Each HIP register segment is addressed through a 4KB aperture window. Each segment
 * has more than 4KB of register space, so a separate index is programmed to
 * HIP_INDEX_REG0 or HIP_INDEX_REG1, based on the segment index, to set the upper 2
 * address bits that point the 4KB window contained in the full segment register
 * space.
 */
#define _HIP_INDEX_REG0					0x1010A0
#define _HIP_INDEX_REG1					0x1010A4
#define HIP_INDEX_REG(seg_idx)				_MMIO((seg_idx) < 4 ? _HIP_INDEX_REG0 : \
									      _HIP_INDEX_REG1)
#define _HIP_INDEX_SHIFT(seg_idx)			(8 * ((seg_idx) % 4))
#define HIP_INDEX_VAL(seg_idx, val)			((val) << _HIP_INDEX_SHIFT(seg_idx))

/**
 * intel_dkl_phy_init - initialize Dekel PHY
 * @display: display device instance
 */
void intel_dkl_phy_init(struct intel_display *display)
{
	spin_lock_init(&display->hip_reg.lock);
}

static void
hip_reg_set_bank_idx(struct intel_display *display, struct intel_hip_reg reg)
{
	int seg_idx = HIP_REG_SEG_IDX(reg);

	if (drm_WARN_ON(display->drm,
			seg_idx < 0 || seg_idx >= HIP_REG_SEG_NUM))
		return;

	intel_de_write(display,
		       HIP_INDEX_REG(seg_idx),
		       HIP_INDEX_VAL(seg_idx, reg.bank_idx));
}

/**
 * intel_dkl_phy_read - read a Dekel PHY register
 * @display: intel_display device instance
 * @reg: Dekel PHY register
 *
 * Read the @reg Dekel PHY register.
 *
 * Returns the read value.
 */
u32
intel_dkl_phy_read(struct intel_display *display, struct intel_hip_reg reg)
{
	u32 val;

	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	val = intel_de_read(display, HIP_REG_MMIO(reg));

	spin_unlock(&display->hip_reg.lock);

	return val;
}

/**
 * intel_dkl_phy_write - write a Dekel PHY register
 * @display: intel_display device instance
 * @reg: Dekel PHY register
 * @val: value to write
 *
 * Write @val to the @reg Dekel PHY register.
 */
void
intel_dkl_phy_write(struct intel_display *display, struct intel_hip_reg reg, u32 val)
{
	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	intel_de_write(display, HIP_REG_MMIO(reg), val);

	spin_unlock(&display->hip_reg.lock);
}

/**
 * intel_dkl_phy_rmw - read-modify-write a Dekel PHY register
 * @display: display device instance
 * @reg: Dekel PHY register
 * @clear: mask to clear
 * @set: mask to set
 *
 * Read the @reg Dekel PHY register, clearing then setting the @clear/@set bits in it, and writing
 * this value back to the register if the value differs from the read one.
 */
void
intel_dkl_phy_rmw(struct intel_display *display, struct intel_hip_reg reg, u32 clear, u32 set)
{
	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	intel_de_rmw(display, HIP_REG_MMIO(reg), clear, set);

	spin_unlock(&display->hip_reg.lock);
}

/**
 * intel_dkl_phy_posting_read - do a posting read from a Dekel PHY register
 * @display: display device instance
 * @reg: Dekel PHY register
 *
 * Read the @reg Dekel PHY register without returning the read value.
 */
void
intel_dkl_phy_posting_read(struct intel_display *display, struct intel_hip_reg reg)
{
	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	intel_de_posting_read(display, HIP_REG_MMIO(reg));

	spin_unlock(&display->hip_reg.lock);
}
