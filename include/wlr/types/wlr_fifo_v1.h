/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FIFO_V1_H
#define WLR_TYPES_WLR_FIFO_V1_H

#include <wayland-server-core.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>

struct wlr_fifo_manager_v1_new_fifo_event {
	struct wlr_fifo_v1 *fifo;
};

struct wlr_fifo_manager_v1 {
	struct wl_global *global;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal new_fifo;

		/**
		 * Signals that the fifo manager is being destroyed.
		 */
		struct wl_signal destroy;
	} events;
};

struct wlr_fifo_v1_surface_state {
	/*
	 * This field is used to set the fifo barrier on the surface.
	 * Set when the client makes a .set_barrier request. */
	bool set_barrier;
	/*
	 * This field is used to lock a commit until the fifo barrier on the surface is cleared.
	 * Set when the client makes a .wait_barrier request. */
	bool wait_barrier;
};

struct wlr_fifo_v1 {
	struct wlr_fifo_manager_v1 *fifo_manager;

	struct wl_resource *resource;
	struct wlr_addon addon;
	struct wlr_surface_synced synced;

	struct wlr_surface *surface;
	struct wlr_output *output;

	struct wl_listener surface_client_commit;
	struct wl_listener surface_commit;
	struct wl_listener output_commit;
	struct wl_listener output_destroy;
	struct wl_listener fifo_manager_destroy;

	struct wlr_fifo_v1_surface_state current, pending;
	bool barrier_init;

	/* list of commit requests waiting on the fifo barrier */
	struct wl_list commits; // fifo_commit.link

	struct {
		/**
		 * Signals that the fifo object is being destroyed.
		 */
		struct wl_signal destroy;
	} events;

	void *private;
};

/**
 * Used to set the output from which to clear the fifo barrier when latching onto new content (ie,
 * when the output receives a commit).
 */
void wlr_fifo_v1_set_output(struct wlr_fifo_v1 *fifo, struct wlr_output *output);

/**
 * Create the wp_fifo_manager_v1_interface global, which can be used by clients to
 * queue commits on a wl_surface for presentation.
 */
struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(struct wl_display *display,
	uint32_t version);

#endif
