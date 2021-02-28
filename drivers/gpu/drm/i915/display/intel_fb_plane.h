/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_FB_PLANE_H__
#define __INTEL_FB_PLANE_H__

#include <linux/types.h>

struct drm_framebuffer;
struct intel_plane_state;
struct drm_i915_private;

bool intel_fb_plane_is_semiplanar_uv(const struct drm_framebuffer *fb, int fb_plane);
bool intel_fb_plane_is_ccs(const struct drm_framebuffer *fb, int fb_plane);
bool intel_fb_plane_is_aux(const struct drm_framebuffer *fb, int fb_plane);
bool intel_fb_plane_is_gen12_ccs(const struct drm_framebuffer *fb, int fb_plane);
bool intel_fb_plane_is_gen12_ccs_cc(const struct drm_framebuffer *fb, int plane);
bool intel_fb_plane_is_linear(const struct drm_framebuffer *fb, int fb_plane);

int intel_fb_plane_main_to_ccs(const struct drm_framebuffer *fb, int ccs_plane);
int intel_fb_plane_ccs_to_main(const struct drm_framebuffer *fb, int ccs_plane);
int intel_fb_plane_main_to_aux(const struct drm_framebuffer *fb, int main_plane);

int intel_fb_plane_gen12_ccs_aux_stride(const struct drm_framebuffer *fb, int ccs_plane);
unsigned int intel_fb_plane_tile_width_bytes(const struct drm_framebuffer *fb, int fb_plane);
unsigned int intel_fb_plane_tile_height(const struct drm_framebuffer *fb, int fb_plane);
void intel_fb_plane_tile_dims(const struct drm_framebuffer *fb, int fb_plane,
			 unsigned int *tile_width, unsigned int *tile_height);
unsigned int intel_fb_plane_tile_row_size(const struct drm_framebuffer *fb, int fb_plane);
unsigned int intel_fb_plane_align_height(const struct drm_framebuffer *fb,
			    int fb_plane, unsigned int height);

unsigned int intel_fb_plane_cursor_alignment(const struct drm_i915_private *i915);
unsigned int intel_fb_plane_surf_alignment(const struct drm_framebuffer *fb, int fb_plane);


int intel_fb_plane_pitch(const struct drm_framebuffer *fb, int fb_plane, unsigned int rotation);
u32 intel_fb_plane_stride_alignment(const struct drm_framebuffer *fb, int fb_plane);

void intel_fb_plane_get_subsampling(const struct drm_framebuffer *fb, int fb_plane, int *hsub, int *vsub);
void intel_fb_plane_dims(const struct drm_framebuffer *fb, int fb_plane, int *width, int *height);

u32 intel_fb_plane_adjust_aligned_offset(const struct intel_plane_state *state, int fb_plane,
					 u32 old_offset, u32 new_offset,
					 int *x, int *y);
u32 intel_fb_plane_compute_aligned_offset(const struct intel_plane_state *state, int fb_plane,
					  int *x, int *y);

int intel_fb_plane_setup_normal_view(struct drm_framebuffer *fb, int fb_plane, int *x, int *y, u32 *offset, u32 *size);
int intel_fb_plane_setup_rotated_view(struct drm_framebuffer *fb, int fb_plane, int x, int y, int offset,
				      int gtt_offset_rotated);
u32 intel_fb_plane_setup_remap_state(const struct drm_framebuffer *fb, int fb_plane,
				     int src_x, int src_y, int src_w, int src_h,
				     int gtt_offset,
				     struct intel_plane_state *plane_state);

#endif /* __INTEL_FB_PLANE_H__ */
