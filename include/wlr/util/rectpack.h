/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_UTIL_RECTPACK_H
#define WLR_UTIL_RECTPACK_H

#include <pixman.h>
#include <wayland-server-protocol.h>

#include <wlr/util/box.h>
#include <wlr/util/edges.h>

struct wlr_layer_surface_v1;

struct wlr_rectpack_rules {
	// If true, the corresponding side will be stretched to take all available area
	bool grow_width, grow_height;
};

/**
 * Place a rectangle within bounds so that it doesn't intersect with the
 * exclusive region.
 *
 * exclusive may be NULL.
 *
 * Returns false if there's not enough space or on memory allocation error.
 */
bool wlr_rectpack_place(const struct wlr_box *bounds, pixman_region32_t *exclusive,
		const struct wlr_box *box, struct wlr_rectpack_rules *rules, struct wlr_box *out);

/**
 * Place a struct wlr_layer_surface_v1 within bounds so that it doesn't
 * intersect with the exclusive region. If the layer surface has exclusive zone,
 * the corresponding area will be added to the exclusive region.
 *
 * Returns false if there's not enough space or on memory allocation error, in
 * which case the exclusive region is left intact.
 */
bool wlr_rectpack_place_wlr_layer_surface_v1(const struct wlr_box *bounds,
		pixman_region32_t *exclusive, struct wlr_layer_surface_v1 *surface, struct wlr_box *out);


#endif
