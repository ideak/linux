/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_HIP_REG_H__
#define __INTEL_HIP_REG_H__

#include <linux/types.h>

#include "intel_hip_reg_defs.h"

struct intel_display;

void intel_hip_reg_init(struct intel_display *display);
u32
intel_hip_reg_read(struct intel_display *display, struct intel_hip_reg reg);
void
intel_hip_reg_write(struct intel_display *display, struct intel_hip_reg reg, u32 val);
void
intel_hip_reg_rmw(struct intel_display *display, struct intel_hip_reg reg, u32 clear, u32 set);
void
intel_hip_reg_posting_read(struct intel_display *display, struct intel_hip_reg reg);

#endif /* __INTEL_HIP_REG_H__ */
