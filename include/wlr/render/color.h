/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_COLOR_H
#define WLR_RENDER_COLOR_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * A color transformation formula, which maps a linear color space with
 * sRGB primaries to an output color space.
 *
 * For ease of use, this type is heap allocated and reference counted.
 * Use wlr_color_transform_ref()/wlr_color_transform_unref(). The initial reference
 * count after creation is 1.
 *
 * Color transforms are immutable; their type/parameters should not be changed,
 * and this API provides no functions to modify them after creation.
 *
 * This formula may be implemented using a 3d look-up table, or some other
 * means.
 */
struct wlr_color_transform;

/**
 * Initialize a color transformation to convert linear
 * (with sRGB(?) primaries) to an ICC profile. Returns NULL on failure.
 */
struct wlr_color_transform *wlr_color_transform_init_linear_to_icc(
	const void *data, size_t size);

/**
 * Initialize a color transformation to apply sRGB encoding.
 * Returns NULL on failure.
 */
struct wlr_color_transform *wlr_color_transform_init_srgb(void);

/**
 * Increase the reference count of the color transform by 1.
 */
struct wlr_color_transform *wlr_color_transform_ref(struct wlr_color_transform *tr);

/**
 * Reduce the reference count of the color transform by 1; freeing it and
 * all associated resources when the reference count hits zero.
 */
void wlr_color_transform_unref(struct wlr_color_transform *tr);

/**
 * Creates a color transform based on a gamma ramp.
 */
struct wlr_color_transform *wlr_color_transform_create_from_gamma_lut(
	size_t ramp_size, const uint16_t *r, const uint16_t *g, const uint16_t *b);

/**
 * Composes two color transforms. This allows compositing multiple color transforms
 * over each other to produce one instead of needing to sample from all color
 * transforms.
 *
 * Note that when compositing a 3d lut transform with an SRGB transform, the result
 * will be lossy as 3d lut cannot losslessly encode SRGB.
 */
struct wlr_color_transform *wlr_color_transform_compose(
	const struct wlr_color_transform *a, const struct wlr_color_transform *b);

#endif
