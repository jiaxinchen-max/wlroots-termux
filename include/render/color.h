#ifndef RENDER_COLOR_H
#define RENDER_COLOR_H

#include <stdint.h>
#include <wlr/util/addon.h>

enum wlr_color_transform_type {
	COLOR_TRANSFORM_SRGB,
	COLOR_TRANSFORM_LUT_3D,
	COLOR_TRANSFORM_LUT_3x1D,
};

struct wlr_color_transform {
	int ref_count;
	struct wlr_addon_set addons; // per-renderer helper state

	enum wlr_color_transform_type type;
};

/**
 * The formula is approximated via a 3D look-up table. A 3D LUT is a
 * three-dimensional array where each element is an RGB triplet. The flat lut_3d
 * array has a length of dim_len³.
 *
 * Color channel values in the range [0.0, 1.0] are mapped linearly to
 * 3D LUT indices such that 0.0 maps exactly to the first element and 1.0 maps
 * exactly to the last element in each dimension.
 *
 * The offset of the RGB triplet given red, green and blue indices r_index,
 * g_index and b_index is:
 *
 *     offset = 3 * (r_index + dim_len * g_index + dim_len * dim_len * b_index)
 */
struct wlr_color_transform_lut3d {
	struct wlr_color_transform base;

	float *lut_3d;
	size_t dim_len;
};

/**
 * This is a color transform that is specified by three seperate ramps that
 * modify the RGB values individually. This means that this color transform type
 * cannot be used to create a transform that can infruence color channels
 * depending on the values of other color channels.
 *
 * This color transform is modeled off of the wlr-gamma-control-unstable-v1
 * wayland protocol.
 *
 * The memory layout of this type requires that the r, g, b color channel ramps
 * are inline in memory. A ramp value can be retrieved from memory:
 *
 *     offset = dim_len * rgb_channel_index + ramp_index
 *
 * This is an offset into the `r` pointer and can be used to retrieve the ramps
 * from the other channels as the ramps are linear in memory. The three pointers
 * are given for convenience.
 *
 * Note that when freeing the color transform, only the `r` channel is freed as
 * a it's expected that a single malloc will allocate all three channels at once.
 */
struct wlr_color_transform_lut3x1d {
	struct wlr_color_transform base;

	uint16_t *r;
	uint16_t *g;
	uint16_t *b;
	size_t ramp_size;
};

/**
 * Gets a wlr_color_transform_lut3d from a generic wlr_color_transform.
 * Asserts that the base type is COLOR_TRANSFORM_LUT_3D
 */
struct wlr_color_transform_lut3d *wlr_color_transform_lut3d_from_base(
	struct wlr_color_transform *tr);

/**
 * Gets a wlr_color_transform_lut3x1d from a generic wlr_color_transform.
 * Asserts that the base type is COLOR_TRANSFORM_LUT_3x1D
 */
struct wlr_color_transform_lut3x1d *wlr_color_transform_lut3x1d_from_base(
	struct wlr_color_transform *tr);

#endif
