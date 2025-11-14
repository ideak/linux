/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_HIP_REG_DEFS_H__
#define __INTEL_HIP_REG_DEFS_H__

#include <linux/types.h>

#include "intel_display_reg_defs.h"

struct intel_dkl_phy_reg {
	u32 reg:24;
	u32 bank_idx:4;
};

#define _DKL_PHY1_BASE					0x168000
#define _DKL_PHY2_BASE					0x169000
#define _DKL_PHY3_BASE					0x16A000
#define _DKL_PHY4_BASE					0x16B000
#define _DKL_PHY5_BASE					0x16C000
#define _DKL_PHY6_BASE					0x16D000

#define DKL_REG_TC_PORT(__reg) \
	(TC_PORT_1 + ((__reg).reg - _DKL_PHY1_BASE) / (_DKL_PHY2_BASE - _DKL_PHY1_BASE))

/* DEKEL PHY MMIO Address = Phy base + (internal address & ~index_mask) */
#define DKL_REG_MMIO(__reg)				_MMIO((__reg).reg)

#define _DKL_REG_PHY_BASE(tc_port)			_PORT(tc_port, \
							      _DKL_PHY1_BASE, \
							      _DKL_PHY2_BASE)

#define _DKL_BANK_SHIFT					12
#define _DKL_REG_BANK_OFFSET(phy_offset) \
	((phy_offset) & ((1 << _DKL_BANK_SHIFT) - 1))
#define _DKL_REG_BANK_IDX(phy_offset) \
	(((phy_offset) >> _DKL_BANK_SHIFT) & 0xf)

#define _DKL_REG(tc_port, phy_offset)	\
	((const struct intel_dkl_phy_reg) { \
		.reg = _DKL_REG_PHY_BASE(tc_port) + \
		       _DKL_REG_BANK_OFFSET(phy_offset), \
		.bank_idx = _DKL_REG_BANK_IDX(phy_offset), \
	})

#endif /* __INTEL_HIP_REG_DEFS_H__ */
