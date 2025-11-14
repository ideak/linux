// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "intel_de.h"
#include "intel_display.h"
#include "intel_hip_reg.h"

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
 * intel_hip_reg_init - initialize HIP
 * @display: display device instance
 */
void intel_hip_reg_init(struct intel_display *display)
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
 * intel_hip_reg_read - read a HIP register
 * @display: intel_display device instance
 * @reg: HIP register
 *
 * Read the @reg HIP register.
 *
 * Returns the read value.
 */
u32
intel_hip_reg_read(struct intel_display *display, struct intel_hip_reg reg)
{
	u32 val;

	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	val = intel_de_read(display, HIP_REG_MMIO(reg));

	spin_unlock(&display->hip_reg.lock);

	return val;
}

/**
 * intel_hip_reg_write - write a HIP register
 * @display: intel_display device instance
 * @reg: HIP register
 * @val: value to write
 *
 * Write @val to the @reg HIP register.
 */
void
intel_hip_reg_write(struct intel_display *display, struct intel_hip_reg reg, u32 val)
{
	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	intel_de_write(display, HIP_REG_MMIO(reg), val);

	spin_unlock(&display->hip_reg.lock);
}

/**
 * intel_hip_reg_rmw - read-modify-write a HIP register
 * @display: display device instance
 * @reg: HIP register
 * @clear: mask to clear
 * @set: mask to set
 *
 * Read the @reg HIP register, clearing then setting the @clear/@set bits in it, and writing
 * this value back to the register if the value differs from the read one.
 */
void
intel_hip_reg_rmw(struct intel_display *display, struct intel_hip_reg reg, u32 clear, u32 set)
{
	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	intel_de_rmw(display, HIP_REG_MMIO(reg), clear, set);

	spin_unlock(&display->hip_reg.lock);
}

/**
 * intel_hip_reg_posting_read - do a posting read from a HIP register
 * @display: display device instance
 * @reg: HIP register
 *
 * Read the @reg HIP register without returning the read value.
 */
void
intel_hip_reg_posting_read(struct intel_display *display, struct intel_hip_reg reg)
{
	spin_lock(&display->hip_reg.lock);

	hip_reg_set_bank_idx(display, reg);
	intel_de_posting_read(display, HIP_REG_MMIO(reg));

	spin_unlock(&display->hip_reg.lock);
}
