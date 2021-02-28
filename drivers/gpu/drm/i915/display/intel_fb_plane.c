// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "intel_display_types.h"
#include "intel_fb_plane.h"
#include "skl_universal_plane.h"

bool
intel_fb_plane_is_semiplanar_uv(const struct drm_framebuffer *fb, int fb_plane)
{
	return intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier) &&
	       fb_plane == 1;
}

bool
intel_fb_plane_is_ccs(const struct drm_framebuffer *fb, int fb_plane)
{
	if (!is_ccs_modifier(fb->modifier))
		return false;

	return fb_plane >= fb->format->num_planes / 2;
}

bool
intel_fb_plane_is_aux(const struct drm_framebuffer *fb, int fb_plane)
{
	if (is_ccs_modifier(fb->modifier))
		return intel_fb_plane_is_ccs(fb, fb_plane);

	return fb_plane == 1;
}

bool
intel_fb_plane_is_gen12_ccs(const struct drm_framebuffer *fb, int fb_plane)
{
	return is_gen12_ccs_modifier(fb->modifier) && intel_fb_plane_is_ccs(fb, fb_plane);
}

bool
intel_fb_plane_is_gen12_ccs_cc(const struct drm_framebuffer *fb, int plane)
{
	return fb->modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC &&
	       plane == 2;
}

bool
intel_fb_plane_is_linear(const struct drm_framebuffer *fb, int fb_plane)
{
	return fb->modifier == DRM_FORMAT_MOD_LINEAR || intel_fb_plane_is_gen12_ccs(fb, fb_plane);
}

int intel_fb_plane_main_to_ccs(const struct drm_framebuffer *fb, int ccs_plane);
int intel_fb_plane_ccs_to_main(const struct drm_framebuffer *fb, int ccs_plane);
int intel_fb_plane_main_to_aux(const struct drm_framebuffer *fb, int main_plane);

int
intel_fb_plane_gen12_ccs_aux_stride(const struct drm_framebuffer *fb, int ccs_plane)
{
	return DIV_ROUND_UP(fb->pitches[intel_fb_plane_ccs_to_main(fb, ccs_plane)], 512) * 64;
}

unsigned int
intel_fb_plane_tile_width_bytes(const struct drm_framebuffer *fb, int fb_plane);

unsigned int
intel_fb_plane_tile_height(const struct drm_framebuffer *fb, int fb_plane)
{
	if (intel_fb_plane_is_gen12_ccs(fb, fb_plane))
		return 1;

	return intel_tile_size(to_i915(fb->dev)) /
		intel_fb_plane_tile_width_bytes(fb, fb_plane);
}

void
intel_fb_plane_tile_dims(const struct drm_framebuffer *fb, int fb_plane,
			 unsigned int *tile_width, unsigned int *tile_height);

unsigned int
intel_fb_plane_tile_row_size(const struct drm_framebuffer *fb, int fb_plane)
{
	unsigned int tile_width, tile_height;

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);

	return fb->pitches[fb_plane] * tile_height;
}

unsigned int
intel_fb_plane_align_height(const struct drm_framebuffer *fb,
			    int fb_plane, unsigned int height)
{
	unsigned int tile_height = intel_fb_plane_tile_height(fb, fb_plane);

	return ALIGN(height, tile_height);
}


unsigned int
intel_fb_plane_tile_width_bytes(const struct drm_framebuffer *fb, int fb_plane)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	unsigned int cpp = fb->format->cpp[fb_plane];

	switch (fb->modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return intel_tile_size(i915);
	case I915_FORMAT_MOD_X_TILED:
		if (IS_GEN(i915, 2))
			return 128;
		else
			return 512;
	case I915_FORMAT_MOD_Y_TILED_CCS:
		if (intel_fb_plane_is_ccs(fb, fb_plane))
			return 128;
		fallthrough;
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS:
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC:
	case I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS:
		if (intel_fb_plane_is_ccs(fb, fb_plane))
			return 64;
		fallthrough;
	case I915_FORMAT_MOD_Y_TILED:
		if (IS_GEN(i915, 2) || HAS_128_BYTE_Y_TILING(i915))
			return 128;
		else
			return 512;
	case I915_FORMAT_MOD_Yf_TILED_CCS:
		if (intel_fb_plane_is_ccs(fb, fb_plane))
			return 128;
		fallthrough;
	case I915_FORMAT_MOD_Yf_TILED:
		switch (cpp) {
		case 1:
			return 64;
		case 2:
		case 4:
			return 128;
		case 8:
		case 16:
			return 256;
		default:
			MISSING_CASE(cpp);
			return cpp;
		}
		break;
	default:
		MISSING_CASE(fb->modifier);
		return cpp;
	}
}

u32
intel_fb_plane_stride_alignment(const struct drm_framebuffer *fb, int fb_plane)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	u32 tile_width;

	if (intel_fb_plane_is_linear(fb, fb_plane)) {
		u32 max_stride = intel_plane_fb_max_stride(i915,
							   fb->format->format,
							   fb->modifier);

		/*
		 * To make remapping with linear generally feasible
		 * we need the stride to be page aligned.
		 */
		if (fb->pitches[fb_plane] > max_stride &&
		    !is_ccs_modifier(fb->modifier))
			return intel_tile_size(i915);
		else
			return 64;
	}

	tile_width = intel_fb_plane_tile_width_bytes(fb, fb_plane);
	if (is_ccs_modifier(fb->modifier)) {
		/*
		 * Display WA #0531: skl,bxt,kbl,glk
		 *
		 * Render decompression and plane width > 3840
		 * combined with horizontal panning requires the
		 * plane stride to be a multiple of 4. We'll just
		 * require the entire fb to accommodate that to avoid
		 * potential runtime errors at plane configuration time.
		 */
		if (IS_GEN(i915, 9) && fb_plane == 0 && fb->width > 3840)
			tile_width *= 4;
		/*
		 * The main surface pitch must be padded to a multiple of four
		 * tile widths.
		 */
		else if (INTEL_GEN(i915) >= 12)
			tile_width *= 4;
	}
	return tile_width;
}

unsigned int intel_fb_plane_cursor_alignment(const struct drm_i915_private *i915)
{
	if (IS_I830(i915))
		return 16 * 1024;
	else if (IS_I85X(i915))
		return 256;
	else if (IS_I845G(i915) || IS_I865G(i915))
		return 32;
	else
		return 4 * 1024;
}

static unsigned int intel_linear_alignment(const struct drm_i915_private *i915)
{
	if (INTEL_GEN(i915) >= 9)
		return 256 * 1024;
	else if (IS_I965G(i915) || IS_I965GM(i915) ||
		 IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915))
		return 128 * 1024;
	else if (INTEL_GEN(i915) >= 4)
		return 4 * 1024;
	else
		return 0;
}

unsigned int intel_fb_plane_surf_alignment(const struct drm_framebuffer *fb,
					   int fb_plane)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);

	/* AUX_DIST needs only 4K alignment */
	if ((INTEL_GEN(i915) < 12 && intel_fb_plane_is_aux(fb, fb_plane)) ||
	    intel_fb_plane_is_ccs(fb, fb_plane))
		return 4096;

	switch (fb->modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return intel_linear_alignment(i915);
	case I915_FORMAT_MOD_X_TILED:
		if (has_async_flips(i915))
			return 256 * 1024;
		return 0;
	case I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS:
		if (intel_fb_plane_is_semiplanar_uv(fb, fb_plane))
			return intel_fb_plane_tile_row_size(fb, fb_plane);
		fallthrough;
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS:
	case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC:
		return 16 * 1024;
	case I915_FORMAT_MOD_Y_TILED_CCS:
	case I915_FORMAT_MOD_Yf_TILED_CCS:
	case I915_FORMAT_MOD_Y_TILED:
		if (INTEL_GEN(i915) >= 12 &&
		    intel_fb_plane_is_semiplanar_uv(fb, fb_plane))
			return intel_fb_plane_tile_row_size(fb, fb_plane);
		fallthrough;
	case I915_FORMAT_MOD_Yf_TILED:
		return 1 * 1024 * 1024;
	default:
		MISSING_CASE(fb->modifier);
		return 0;
	}
}


int intel_fb_plane_ccs_to_main(const struct drm_framebuffer *fb, int ccs_plane)
{
	drm_WARN_ON(fb->dev, !is_ccs_modifier(fb->modifier) ||
		    ccs_plane < fb->format->num_planes / 2);

	if (intel_fb_plane_is_gen12_ccs_cc(fb, ccs_plane))
		return 0;

	return ccs_plane - fb->format->num_planes / 2;
}

int
intel_fb_plane_main_to_ccs(const struct drm_framebuffer *fb, int main_plane)
{
	drm_WARN_ON(fb->dev, !is_ccs_modifier(fb->modifier) ||
		    (main_plane && main_plane >= fb->format->num_planes / 2));

	return fb->format->num_planes / 2 + main_plane;
}

int
intel_fb_plane_main_to_aux(const struct drm_framebuffer *fb, int main_plane)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);

	if (is_ccs_modifier(fb->modifier))
		return intel_fb_plane_main_to_ccs(fb, main_plane);
	else if (INTEL_GEN(i915) < 11 &&
		 intel_format_info_is_yuv_semiplanar(fb->format, fb->modifier))
		return 1;
	else
		return 0;
}

/* Return the tile dimensions in pixel units */
void intel_fb_plane_tile_dims(const struct drm_framebuffer *fb, int fb_plane,
			      unsigned int *tile_width,
			      unsigned int *tile_height)
{
	unsigned int tile_width_bytes = intel_fb_plane_tile_width_bytes(fb, fb_plane);
	unsigned int cpp = fb->format->cpp[fb_plane];

	*tile_width = tile_width_bytes / cpp;
	*tile_height = intel_fb_plane_tile_height(fb, fb_plane);
}

void intel_fb_plane_get_subsampling(const struct drm_framebuffer *fb, int fb_plane, int *hsub, int *vsub)
{
	int main_plane;

	if (fb_plane == 0) {
		*hsub = 1;
		*vsub = 1;

		return;
	}

	/*
	 * TODO: Deduct the subsampling from the char block for all CCS
	 * formats and planes.
	 */
	if (!intel_fb_plane_is_gen12_ccs(fb, fb_plane)) {
		*hsub = fb->format->hsub;
		*vsub = fb->format->vsub;

		return;
	}

	main_plane = intel_fb_plane_ccs_to_main(fb, fb_plane);
	*hsub = drm_format_info_block_width(fb->format, fb_plane) /
		drm_format_info_block_width(fb->format, main_plane);

	/*
	 * The min stride check in the core framebuffer_check() function
	 * assumes that format->hsub applies to every plane except for the
	 * first plane. That's incorrect for the CCS AUX plane of the first
	 * plane, but for the above check to pass we must define the block
	 * width with that subsampling applied to it. Adjust the width here
	 * accordingly, so we can calculate the actual subsampling factor.
	 */
	if (main_plane == 0)
		*hsub *= fb->format->hsub;

	*vsub = 32;
}

void
intel_fb_plane_dims(const struct drm_framebuffer *fb, int fb_plane, int *width, int *height)
{
	int main_plane = intel_fb_plane_is_ccs(fb, fb_plane) ?
			 intel_fb_plane_ccs_to_main(fb, fb_plane) : 0;
	int main_hsub, main_vsub;
	int hsub, vsub;

	intel_fb_plane_get_subsampling(fb, main_plane, &main_hsub, &main_vsub);
	intel_fb_plane_get_subsampling(fb, fb_plane, &hsub, &vsub);
	*width = fb->width / main_hsub / hsub;
	*height = fb->height / main_vsub / vsub;
}

int intel_fb_plane_pitch(const struct drm_framebuffer *fb, int fb_plane, unsigned int rotation)
{
	if (drm_rotation_90_or_270(rotation))
		return to_intel_framebuffer(fb)->rotated[fb_plane].pitch;
	else
		return fb->pitches[fb_plane];
}

static int intel_fb_plane_normal_view_size(const struct drm_framebuffer *fb, int fb_plane, int x, int y)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	unsigned int cpp = fb->format->cpp[fb_plane];
	unsigned int fb_plane_width;
	unsigned int fb_plane_height;
	unsigned int stride_bytes = fb->pitches[fb_plane];
	unsigned int tile_width, tile_height;
	unsigned int stride_tiles;
	unsigned int tile_rows;
	unsigned int size;

	intel_fb_plane_dims(fb, fb_plane, &fb_plane_width, &fb_plane_height);

	if (intel_fb_plane_is_linear(fb, fb_plane))
		return DIV_ROUND_UP((y + fb_plane_height) * stride_bytes + x * cpp, intel_tile_size(i915));

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);

	stride_tiles = DIV_ROUND_UP(stride_bytes, tile_width * cpp);
	tile_rows = DIV_ROUND_UP(y + fb_plane_height, tile_height);

	/* how many tiles does this plane need */
	size = stride_tiles * tile_rows;

	/* If the plane isn't horizontally tile aligned, we need one more tile. */
	if (x != 0)
		size++;

	return size;
}

static int intel_fb_plane_remapped_view_info(const struct drm_framebuffer *fb, int fb_plane, int offset,
					     int x, int y, struct intel_remapped_plane_info *plane_info)
{
	unsigned int fb_plane_width;
	unsigned int fb_plane_height;
	unsigned int cpp = fb->format->cpp[fb_plane];
	unsigned int tile_width, tile_height;

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);
	intel_fb_plane_dims(fb, fb_plane, &fb_plane_width, &fb_plane_height);

	plane_info->offset = offset;
	plane_info->stride = DIV_ROUND_UP(fb->pitches[fb_plane], tile_width * cpp);
	plane_info->width = DIV_ROUND_UP(x + fb_plane_width, tile_width);
	plane_info->height = DIV_ROUND_UP(y + fb_plane_height, tile_height);

	return plane_info->width * plane_info->height;
}

static int intel_fb_plane_rotated_view_info(const struct drm_framebuffer *fb, int fb_plane,
					    int offset,
					    int *x, int *y, struct intel_remapped_plane_info *plane_info)
{
	unsigned int fb_plane_width;
	unsigned int fb_plane_height;
	unsigned int tile_width, tile_height;
	struct drm_rect r;
	int size;

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);
	intel_fb_plane_dims(fb, fb_plane, &fb_plane_width, &fb_plane_height);

	size = intel_fb_plane_remapped_view_info(fb, fb_plane, offset, *x, *y, plane_info);

	/* rotate the x/y offsets to match the GTT view */
	drm_rect_init(&r, *x, *y, fb_plane_width, fb_plane_height);
	drm_rect_rotate(&r,
			plane_info->width * tile_width,
			plane_info->height * tile_height,
			DRM_MODE_ROTATE_270);
	*x = r.x1;
	*y = r.y1;

	return size;
}

static u32 intel_adjust_tile_offset(int *x, int *y,
				    unsigned int tile_width,
				    unsigned int tile_height,
				    unsigned int tile_size,
				    unsigned int pitch_tiles,
				    u32 old_offset,
				    u32 new_offset)
{
	unsigned int pitch_pixels = pitch_tiles * tile_width;
	unsigned int tiles;

	WARN_ON(old_offset & (tile_size - 1));
	WARN_ON(new_offset & (tile_size - 1));
	WARN_ON(new_offset > old_offset);

	tiles = (old_offset - new_offset) / tile_size;

	*y += tiles / pitch_tiles * tile_height;
	*x += tiles % pitch_tiles * tile_width;

	/* minimize x in case it got needlessly big */
	*y += *x / pitch_pixels * tile_height;
	*x %= pitch_pixels;

	return new_offset;
}

static u32 intel_adjust_aligned_offset(const struct drm_framebuffer *fb, int fb_plane,
				       unsigned int rotation, unsigned int pitch,
				       u32 old_offset, u32 new_offset,
				       int *x, int *y)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	unsigned int cpp = fb->format->cpp[fb_plane];

	drm_WARN_ON(&i915->drm, new_offset > old_offset);

	if (!intel_fb_plane_is_linear(fb, fb_plane)) {
		unsigned int tile_size, tile_width, tile_height;
		unsigned int pitch_tiles;

		tile_size = intel_tile_size(i915);
		intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);

		if (drm_rotation_90_or_270(rotation)) {
			pitch_tiles = pitch / tile_height;
			swap(tile_width, tile_height);
		} else {
			pitch_tiles = pitch / (tile_width * cpp);
		}

		intel_adjust_tile_offset(x, y, tile_width, tile_height,
					 tile_size, pitch_tiles,
					 old_offset, new_offset);
	} else {
		old_offset += *y * pitch + *x * cpp;

		*y = (old_offset - new_offset) / pitch;
		*x = ((old_offset - new_offset) - *y * pitch) / cpp;
	}

	return new_offset;
}

/*
 * Adjust the tile offset by moving the difference into
 * the x/y offsets.
 */
u32 intel_fb_plane_adjust_aligned_offset(const struct intel_plane_state *state, int fb_plane,
					 u32 old_offset, u32 new_offset,
					 int *x, int *y)
{
	return intel_adjust_aligned_offset(state->hw.fb, fb_plane,
					   state->hw.rotation,
					   state->color_plane[fb_plane].stride,
					   old_offset, new_offset,
					   x, y);
}

/*
 * Computes the aligned offset to the base tile and adjusts
 * x, y. bytes per pixel is assumed to be a power-of-two.
 *
 * In the 90/270 rotated case, x and y are assumed
 * to be already rotated to match the rotated GTT view, and
 * pitch is the tile_height aligned framebuffer height.
 *
 * This function is used when computing the derived information
 * under intel_framebuffer, so using any of that information
 * here is not allowed. Anything under drm_framebuffer can be
 * used. This is why the user has to pass in the pitch since it
 * is specified in the rotated orientation.
 */
static u32 intel_compute_aligned_offset(const struct drm_framebuffer *fb, int fb_plane,
					unsigned int rotation, unsigned int pitch, u32 alignment,
					int *x, int *y)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	unsigned int cpp = fb->format->cpp[fb_plane];
	u32 offset, offset_aligned;

	if (!intel_fb_plane_is_linear(fb, fb_plane)) {
		unsigned int tile_size, tile_width, tile_height;
		unsigned int tile_rows, tiles, pitch_tiles;

		tile_size = intel_tile_size(i915);
		intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);

		if (drm_rotation_90_or_270(rotation)) {
			pitch_tiles = pitch / tile_height;
			swap(tile_width, tile_height);
		} else {
			pitch_tiles = pitch / (tile_width * cpp);
		}

		tile_rows = *y / tile_height;
		*y %= tile_height;

		tiles = *x / tile_width;
		*x %= tile_width;

		offset = (tile_rows * pitch_tiles + tiles) * tile_size;

		offset_aligned = offset;
		if (alignment)
			offset_aligned = rounddown(offset_aligned, alignment);

		intel_adjust_tile_offset(x, y, tile_width, tile_height,
					 tile_size, pitch_tiles,
					 offset, offset_aligned);
	} else {
		offset = *y * pitch + *x * cpp;
		offset_aligned = offset;
		if (alignment) {
			offset_aligned = rounddown(offset_aligned, alignment);
			*y = (offset % alignment) / pitch;
			*x = ((offset % alignment) - *y * pitch) / cpp;
		} else {
			*y = *x = 0;
		}
	}

	return offset_aligned;
}

u32 intel_fb_plane_compute_aligned_offset(const struct intel_plane_state *state,
					  int fb_plane,
					  int *x, int *y)
{
	struct intel_plane *intel_plane = to_intel_plane(state->uapi.plane);
	struct drm_i915_private *i915 = to_i915(intel_plane->base.dev);
	const struct drm_framebuffer *fb = state->hw.fb;
	unsigned int rotation = state->hw.rotation;
	int pitch = state->color_plane[fb_plane].stride;
	u32 alignment;

	if (intel_plane->id == PLANE_CURSOR)
		alignment = intel_fb_plane_cursor_alignment(i915);
	else
		alignment = intel_fb_plane_surf_alignment(fb, fb_plane);

	return intel_compute_aligned_offset(fb, fb_plane, pitch, rotation, alignment, x, y);
}

static int
intel_fb_plane_check_ccs_xy(const struct drm_framebuffer *fb, int fb_plane, int x, int y)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	struct intel_framebuffer *intel_fb = to_intel_framebuffer(fb);
	int main_plane;
	int hsub, vsub;
	int tile_width, tile_height;
	int ccs_x, ccs_y;
	int main_x, main_y;

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);
	intel_fb_plane_get_subsampling(fb, fb_plane, &hsub, &vsub);

	tile_width *= hsub;
	tile_height *= vsub;

	ccs_x = (x * hsub) % tile_width;
	ccs_y = (y * vsub) % tile_height;

	main_plane = intel_fb_plane_ccs_to_main(fb, fb_plane);
	main_x = intel_fb->normal[main_plane].x % tile_width;
	main_y = intel_fb->normal[main_plane].y % tile_height;

	/*
	 * CCS doesn't have its own x/y offset register, so the intra CCS tile
	 * x/y offsets must match between CCS and the main surface.
	 */
	if (main_x != ccs_x || main_y != ccs_y) {
		drm_dbg_kms(&i915->drm,
			      "Bad CCS x/y (main %d,%d ccs %d,%d) full (main %d,%d ccs %d,%d)\n",
			      main_x, main_y,
			      ccs_x, ccs_y,
			      intel_fb->normal[main_plane].x,
			      intel_fb->normal[main_plane].y,
			      x, y);
		return -EINVAL;
	}

	return 0;
}

static int
intel_fb_plane_check_xy(const struct drm_framebuffer *fb, int fb_plane, int x, int y)
{
	struct drm_i915_gem_object *obj = intel_fb_obj(fb);
	unsigned int width;
	unsigned int height;
	int ret;

	if (intel_fb_plane_is_ccs(fb, fb_plane) &&
	    !intel_fb_plane_is_gen12_ccs_cc(fb, fb_plane)) {
		ret = intel_fb_plane_check_ccs_xy(fb, fb_plane, x, y);
		if (ret)
			return ret;
	}

	if (fb_plane != 0)
		return 0;

	if (!i915_gem_object_is_tiled(obj))
		return 0;

	/*
	 * The fence (if used) is aligned to the start of the object so having
	 * the framebuffer wrap around across the edge of the fenced region
	 * doesn't really work. We have no API to configure the fence start
	 * offset within the object (nor could we probably on gen2/3). So it's
	 * just easier if we just require that the fb layout agrees with the
	 * fence layout. We already check that the fb stride matches the fence
	 * stride elsewhere.
	 */
	intel_fb_plane_dims(fb, fb_plane, &width, &height);

	if ((x + width) * fb->format->cpp[fb_plane] > fb->pitches[fb_plane]) {
		drm_dbg_kms(fb->dev, "bad fb plane %d offset: 0x%x\n",
			    fb_plane, fb->offsets[fb_plane]);
		return -EINVAL;
	}

	return 0;
}

/* Convert the fb->offset[] into x/y offsets */
static int intel_fb_plane_offset_to_xy(const struct drm_framebuffer *fb, int fb_plane, int *x, int *y)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	unsigned int height;
	u32 alignment;

	if (INTEL_GEN(i915) >= 12 &&
	    intel_fb_plane_is_semiplanar_uv(fb, fb_plane))
		alignment = intel_fb_plane_tile_row_size(fb, fb_plane);
	else if (fb->modifier != DRM_FORMAT_MOD_LINEAR)
		alignment = intel_tile_size(i915);
	else
		alignment = 0;

	if (alignment != 0 && fb->offsets[fb_plane] % alignment) {
		drm_dbg_kms(&i915->drm,
			    "Misaligned offset 0x%08x for color plane %d\n",
			    fb->offsets[fb_plane], fb_plane);
		return -EINVAL;
	}

	height = drm_framebuffer_plane_height(fb->height, fb, fb_plane);
	height = ALIGN(height, intel_fb_plane_tile_height(fb, fb_plane));

	/* Catch potential overflows early */
	if (add_overflows_t(u32, mul_u32_u32(height, fb->pitches[fb_plane]),
			    fb->offsets[fb_plane])) {
		drm_dbg_kms(&i915->drm,
			    "Bad offset 0x%08x or pitch %d for color plane %d\n",
			    fb->offsets[fb_plane], fb->pitches[fb_plane],
			    fb_plane);
		return -ERANGE;
	}

	*x = 0;
	*y = 0;

	intel_adjust_aligned_offset(fb, fb_plane, DRM_MODE_ROTATE_0,
				    fb->pitches[fb_plane],
				    fb->offsets[fb_plane], 0,
				    x, y);

	return intel_fb_plane_check_xy(fb, fb_plane, *x, *y);
}


int intel_fb_plane_setup_normal_view(struct drm_framebuffer *fb, int fb_plane, int *x, int *y, u32 *offset, u32 *size)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	struct intel_framebuffer *intel_fb = to_intel_framebuffer(fb);
	int tile_size = intel_tile_size(i915);
	unsigned int fb_plane_width;
	unsigned int fb_plane_height;
	unsigned int cpp;
	int ret;

	ret = intel_fb_plane_offset_to_xy(fb, fb_plane, x, y);
	if (ret)
		return ret;

	cpp = fb->format->cpp[fb_plane];
	intel_fb_plane_dims(fb, fb_plane, &fb_plane_width, &fb_plane_height);

	/*
	 * First pixel of the framebuffer from
	 * the start of the normal gtt mapping.
	 */
	intel_fb->normal[fb_plane].x = *x;
	intel_fb->normal[fb_plane].y = *y;

	*offset = intel_compute_aligned_offset(fb, fb_plane,
					       fb->pitches[fb_plane], DRM_MODE_ROTATE_0, tile_size,
					       x, y);
	*offset /= tile_size;

	*size = intel_fb_plane_normal_view_size(fb, fb_plane, *x, *y);

	return 0;
}

/*
 * Setup the rotated view for an FB plane and return the size the GTT mapping
 * requires for this view.
 */
int intel_fb_plane_setup_rotated_view(struct drm_framebuffer *fb, int fb_plane, int x, int y, int offset,
				      int gtt_offset_rotated)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	struct intel_framebuffer *intel_fb = to_intel_framebuffer(fb);
	struct intel_remapped_plane_info *rot_info = &intel_fb->rot_info.plane[fb_plane];
	int tile_size = intel_tile_size(i915);
	unsigned int tile_width, tile_height;
	unsigned int pitch_tiles;
	int rotated_size;

	if (drm_WARN_ON(fb->dev, fb_plane >= ARRAY_SIZE(intel_fb->rot_info.plane)))
		return 0;

	rotated_size = intel_fb_plane_rotated_view_info(fb, fb_plane, offset, &x, &y, rot_info);

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);
	intel_fb->rotated[fb_plane].pitch = rot_info->height * tile_height;

	/* rotate the tile dimensions to match the GTT view */
	pitch_tiles = intel_fb->rotated[fb_plane].pitch / tile_height;
	swap(tile_width, tile_height);

	/*
	 * We only keep the x/y offsets, so push all of the
	 * gtt offset into the x/y offsets.
	 */
	intel_adjust_tile_offset(&x, &y,
				 tile_width, tile_height,
				 tile_size, pitch_tiles,
				 gtt_offset_rotated * tile_size, 0);

	/*
	 * First pixel of the framebuffer from
	 * the start of the rotated gtt mapping.
	 */
	intel_fb->rotated[fb_plane].x = x;
	intel_fb->rotated[fb_plane].y = y;

	return rotated_size;
}

u32 intel_fb_plane_setup_remap_state(const struct drm_framebuffer *fb, int fb_plane,
				     int src_x, int src_y, int src_w, int src_h,
				     int gtt_offset,
				     struct intel_plane_state *plane_state)
{
	struct drm_i915_private *i915 = to_i915(fb->dev);
	struct intel_framebuffer *intel_fb = to_intel_framebuffer(fb);
	unsigned int rotation = plane_state->hw.rotation;
	unsigned int hsub = fb_plane ? fb->format->hsub : 1;
	unsigned int vsub = fb_plane ? fb->format->vsub : 1;
	unsigned int cpp = fb->format->cpp[fb_plane];
	unsigned int tile_size = intel_tile_size(i915);
	unsigned int tile_width, tile_height;
	unsigned int width, height;
	unsigned int pitch_tiles;
	unsigned int x, y;
	u32 offset;
	u32 gtt_size;

	intel_fb_plane_tile_dims(fb, fb_plane, &tile_width, &tile_height);

	x = src_x / hsub;
	y = src_y / vsub;
	width = src_w / hsub;
	height = src_h / vsub;

	/* First pixel of the src viewport from the start of the normal gtt mapping. */
	x += intel_fb->normal[fb_plane].x;
	y += intel_fb->normal[fb_plane].y;

	offset = intel_compute_aligned_offset(fb, fb_plane,
					      fb->pitches[fb_plane], DRM_MODE_ROTATE_0, tile_size,
					      &x, &y);
	offset /= tile_size;

	drm_WARN_ON(&i915->drm, fb_plane >= ARRAY_SIZE(plane_state->view.rotated.plane));

	if (drm_rotation_90_or_270(rotation)) {
		struct intel_remapped_plane_info *info = &plane_state->view.rotated.plane[fb_plane];

		gtt_size = intel_fb_plane_rotated_view_info(fb, fb_plane, offset, &x, &y, info);

		pitch_tiles = info->height;
		plane_state->color_plane[fb_plane].stride = pitch_tiles * tile_height;

		/* rotate the tile dimensions to match the GTT view */
		swap(tile_width, tile_height);
	} else {
		struct intel_remapped_plane_info *info = &plane_state->view.remapped.plane[fb_plane];

		gtt_size = intel_fb_plane_remapped_view_info(fb, fb_plane, offset, x, y, info);

		pitch_tiles = info->width;
		plane_state->color_plane[fb_plane].stride = pitch_tiles * tile_width * cpp;
	}

	/*
	 * We only keep the x/y offsets, so push all of the
	 * gtt offset into the x/y offsets.
	 */
	intel_adjust_tile_offset(&x, &y,
				 tile_width, tile_height,
				 tile_size, pitch_tiles,
				 gtt_offset * tile_size, 0);

	plane_state->color_plane[fb_plane].offset = 0;
	plane_state->color_plane[fb_plane].x = x;
	plane_state->color_plane[fb_plane].y = y;

	return gtt_size;
}

