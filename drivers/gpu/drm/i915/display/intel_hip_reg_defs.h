/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_HIP_REG_DEFS_H__
#define __INTEL_HIP_REG_DEFS_H__

#include <linux/types.h>

#include "intel_display_reg_defs.h"

struct intel_hip_reg {
	u32 reg:24;
	u32 bank_idx:4;
};

#define _HIP_REG_SEG0_BASE				0x168000
#define _HIP_REG_SEG1_BASE				0x169000

#define HIP_REG_SEG_NUM					8

/* HIP REG MMIO Address = HIP segment base + (internal address & ~segment_index_mask) */
#define HIP_REG_MMIO(__reg)				_MMIO((__reg).reg)

#define HIP_REG_SEG_IDX(__reg) \
	(((__reg).reg - _HIP_REG_SEG0_BASE) / (_HIP_REG_SEG1_BASE - _HIP_REG_SEG0_BASE))


#define _HIP_REG_SEG_BASE(__seg_idx)			_PORT(__seg_idx, \
							      _HIP_REG_SEG0_BASE, \
							      _HIP_REG_SEG1_BASE)

#define _HIP_REG_SEG_BANK_SHIFT				12
#define _HIP_REG_SEG_BANK_OFFSET(__offset) \
	((__offset) & ((1 << _HIP_REG_SEG_BANK_SHIFT) - 1))
#define _HIP_REG_SEG_BANK_IDX(__offset) \
	(((__offset) >> _HIP_REG_SEG_BANK_SHIFT) & 0xf)

#define HIP_REG(__seg_idx, __seg_offset)	\
	((const struct intel_hip_reg) { \
		.reg = _HIP_REG_SEG_BASE(__seg_idx) + \
		       _HIP_REG_SEG_BANK_OFFSET(__seg_offset), \
		.bank_idx = _HIP_REG_SEG_BANK_IDX(__seg_offset), \
	})

#endif /* __INTEL_HIP_REG_DEFS_H__ */
