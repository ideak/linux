/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_DKL_PHY_H__
#define __INTEL_DKL_PHY_H__

#include <linux/types.h>

#include "intel_dkl_phy_regs.h"
#include "intel_hip_reg.h"

struct intel_display;

static inline struct intel_hip_reg dkl_phy_to_hip_reg(struct intel_dkl_phy_reg reg)
{
	return (const struct intel_hip_reg){ .reg = reg.reg, .bank_idx = reg.bank_idx };
}

static inline u32
intel_dkl_phy_read(struct intel_display *display, struct intel_dkl_phy_reg reg)
{
	return intel_hip_reg_read(display, dkl_phy_to_hip_reg(reg));
}

static inline void
intel_dkl_phy_write(struct intel_display *display, struct intel_dkl_phy_reg reg, u32 val)
{
	intel_hip_reg_write(display, dkl_phy_to_hip_reg(reg), val);
}

static inline void
intel_dkl_phy_rmw(struct intel_display *display, struct intel_dkl_phy_reg reg, u32 clear, u32 set)
{
	intel_hip_reg_rmw(display, dkl_phy_to_hip_reg(reg), clear, set);
}

static inline void
intel_dkl_phy_posting_read(struct intel_display *display, struct intel_dkl_phy_reg reg)
{
	intel_hip_reg_posting_read(display, dkl_phy_to_hip_reg(reg));
}

#endif /* __INTEL_DKL_PHY_H__ */
