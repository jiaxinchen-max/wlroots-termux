/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_SURFACE_INVALIDATION_V1_H
#define WLR_TYPES_WLR_SURFACE_INVALIDATION_V1_H

#include <wayland-server-core.h>

struct wlr_surface;

struct wlr_surface_invalidation_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	struct wl_listener display_destroy;
};

struct wlr_surface_invalidation_manager_v1 *wlr_surface_invalidation_manager_v1_create(
	struct wl_display *display, uint32_t version);

bool wlr_surface_invalidation_manager_v1_invalidate(
	const struct wlr_surface_invalidation_manager_v1 *manager,
	struct wlr_surface *surface);

#endif
