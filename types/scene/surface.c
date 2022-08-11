#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_presentation_time.h>
#include "types/wlr_scene.h"

static void handle_scene_raster_output_enter(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, output_enter);
	struct wlr_scene_output *output = data;

	wlr_surface_send_enter(surface->surface, output->output);
}

static void handle_scene_raster_output_leave(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, output_leave);
	struct wlr_scene_output *output = data;

	wlr_surface_send_leave(surface->surface, output->output);
}

static void handle_scene_raster_output_present(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, output_present);
	struct wlr_scene_output *scene_output = data;

	if (surface->raster->primary_output == scene_output) {
		struct wlr_scene *root = scene_node_get_root(&surface->raster->node);
		struct wlr_presentation *presentation = root->presentation;

		if (presentation) {
			wlr_presentation_surface_sampled_on_output(
				presentation, surface->surface, scene_output->output);
		}
	}
}

static void handle_scene_raster_frame_done(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, frame_done);
	struct timespec *now = data;

	wlr_surface_send_frame_done(surface->surface, now);
}

static void scene_surface_handle_surface_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, surface_destroy);

	wlr_scene_node_destroy(&surface->raster->node);
}

static void set_raster_with_surface_state(struct wlr_scene_raster *scene_raster,
		struct wlr_surface *surface) {
	struct wlr_surface_state *state = &surface->current;

	struct wlr_fbox src_box;
	wlr_surface_get_buffer_source_box(surface, &src_box);
	wlr_scene_raster_set_source_box(scene_raster, &src_box);

	wlr_scene_raster_set_dest_size(scene_raster, state->width, state->height);
	wlr_scene_raster_set_transform(scene_raster, state->transform);

	struct wlr_raster *raster = surface->raster;

	if (raster) {
		wlr_scene_raster_set_raster_with_damage(scene_raster,
			raster, &surface->buffer_damage);
	} else {
		wlr_scene_raster_set_raster(scene_raster, NULL);
	}
}

static void handle_scene_surface_surface_commit(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *surface =
		wl_container_of(listener, surface, surface_commit);
	struct wlr_scene_raster *scene_raster = surface->raster;

	set_raster_with_surface_state(scene_raster, surface->surface);

	// Even if the surface hasn't submitted damage, schedule a new frame if
	// the client has requested a wl_surface.frame callback. Check if the node
	// is visible. If not, the client will never receive a frame_done event
	// anyway so it doesn't make sense to schedule here.
	int lx, ly;
	bool enabled = wlr_scene_node_coords(&scene_raster->node, &lx, &ly);

	if (!wl_list_empty(&surface->surface->current.frame_callback_list) &&
			surface->raster->primary_output != NULL && enabled) {
		wlr_output_schedule_frame(surface->raster->primary_output->output);
	}
}

static bool scene_raster_point_accepts_input(struct wlr_scene_raster *scene_raster,
		int sx, int sy) {
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_from_raster(scene_raster);

	return wlr_surface_point_accepts_input(scene_surface->surface, sx, sy);
}

static void surface_addon_destroy(struct wlr_addon *addon) {
	struct wlr_scene_surface *surface = wl_container_of(addon, surface, addon);

	wlr_addon_finish(&surface->addon);

	wl_list_remove(&surface->output_enter.link);
	wl_list_remove(&surface->output_leave.link);
	wl_list_remove(&surface->output_present.link);
	wl_list_remove(&surface->frame_done.link);
	wl_list_remove(&surface->surface_destroy.link);
	wl_list_remove(&surface->surface_commit.link);

	free(surface);
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "wlr_scene_surface",
	.destroy = surface_addon_destroy,
};

struct wlr_scene_surface *wlr_scene_surface_from_raster(
		struct wlr_scene_raster *scene_raster) {
	struct wlr_addon *addon = wlr_addon_find(&scene_raster->node.addons,
		scene_raster, &surface_addon_impl);
	if (!addon) {
		return NULL;
	}

	struct wlr_scene_surface *surface = wl_container_of(addon, surface, addon);
	return surface;
}

struct wlr_scene_surface *wlr_scene_surface_create(struct wlr_scene_tree *parent,
		struct wlr_surface *wlr_surface) {
	struct wlr_scene_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		return NULL;
	}

	struct wlr_scene_raster *scene_raster = wlr_scene_raster_create(parent, NULL);
	if (!scene_raster) {
		free(surface);
		return NULL;
	}

	surface->raster = scene_raster;
	surface->surface = wlr_surface;
	scene_raster->point_accepts_input = scene_raster_point_accepts_input;

	surface->output_enter.notify = handle_scene_raster_output_enter;
	wl_signal_add(&scene_raster->events.output_enter, &surface->output_enter);

	surface->output_leave.notify = handle_scene_raster_output_leave;
	wl_signal_add(&scene_raster->events.output_leave, &surface->output_leave);

	surface->output_present.notify = handle_scene_raster_output_present;
	wl_signal_add(&scene_raster->events.output_present, &surface->output_present);

	surface->frame_done.notify = handle_scene_raster_frame_done;
	wl_signal_add(&scene_raster->events.frame_done, &surface->frame_done);

	surface->surface_destroy.notify = scene_surface_handle_surface_destroy;
	wl_signal_add(&wlr_surface->events.destroy, &surface->surface_destroy);

	surface->surface_commit.notify = handle_scene_surface_surface_commit;
	wl_signal_add(&wlr_surface->events.commit, &surface->surface_commit);

	wlr_addon_init(&surface->addon, &scene_raster->node.addons,
		scene_raster, &surface_addon_impl);

	set_raster_with_surface_state(scene_raster, wlr_surface);

	return surface;
}
