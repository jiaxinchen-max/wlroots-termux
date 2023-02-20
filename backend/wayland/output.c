#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wayland-client.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/util/log.h>

#include "backend/wayland.h"
#include "render/pixel_format.h"
#include "render/wlr_renderer.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_MODE |
	WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;

static size_t last_output_num = 0;

static struct wlr_wl_output *get_wl_output_from_output(
		struct wlr_output *wlr_output) {
	assert(wlr_output_is_wl(wlr_output));
	return (struct wlr_wl_output *)wlr_output;
}

static void surface_frame_callback(void *data, struct wl_callback *cb,
		uint32_t time) {
	struct wlr_wl_output *output = data;

	if (cb == NULL) {
		return;
	}

	assert(output->frame_callback == cb);
	wl_callback_destroy(cb);
	output->frame_callback = NULL;

	wlr_output_send_frame(&output->wlr_output);
}

static const struct wl_callback_listener frame_listener = {
	.done = surface_frame_callback
};

static void presentation_feedback_destroy(
		struct wlr_wl_presentation_feedback *feedback) {
	wl_list_remove(&feedback->link);
	wp_presentation_feedback_destroy(feedback->feedback);
	free(feedback);
}

static void presentation_feedback_handle_sync_output(void *data,
		struct wp_presentation_feedback *feedback, struct wl_output *output) {
	// This space is intentionally left blank
}

static void presentation_feedback_handle_presented(void *data,
		struct wp_presentation_feedback *wp_feedback, uint32_t tv_sec_hi,
		uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh_ns,
		uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
	struct wlr_wl_presentation_feedback *feedback = data;

	struct timespec t = {
		.tv_sec = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo,
		.tv_nsec = tv_nsec,
	};
	struct wlr_output_event_present event = {
		.commit_seq = feedback->commit_seq,
		.presented = true,
		.when = &t,
		.seq = ((uint64_t)seq_hi << 32) | seq_lo,
		.refresh = refresh_ns,
		.flags = flags,
	};
	wlr_output_send_present(&feedback->output->wlr_output, &event);

	presentation_feedback_destroy(feedback);
}

static void presentation_feedback_handle_discarded(void *data,
		struct wp_presentation_feedback *wp_feedback) {
	struct wlr_wl_presentation_feedback *feedback = data;

	struct wlr_output_event_present event = {
		.commit_seq = feedback->commit_seq,
		.presented = false,
	};
	wlr_output_send_present(&feedback->output->wlr_output, &event);

	presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener
		presentation_feedback_listener = {
	.sync_output = presentation_feedback_handle_sync_output,
	.presented = presentation_feedback_handle_presented,
	.discarded = presentation_feedback_handle_discarded,
};

void destroy_wl_buffer(struct wlr_wl_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}
	wl_list_remove(&buffer->buffer_destroy.link);
	wl_list_remove(&buffer->link);
	wl_buffer_destroy(buffer->wl_buffer);
	if (!buffer->released) {
		wlr_buffer_unlock(buffer->buffer);
	}
	free(buffer);
}

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {
	struct wlr_wl_buffer *buffer = data;
	buffer->released = true;
	wlr_buffer_unlock(buffer->buffer); // might free buffer
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

static void buffer_handle_buffer_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_wl_buffer *buffer =
		wl_container_of(listener, buffer, buffer_destroy);
	destroy_wl_buffer(buffer);
}

static bool test_buffer(struct wlr_wl_backend *wl,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_dmabuf_attributes dmabuf;
	struct wlr_shm_attributes shm;
	if (wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		return wlr_drm_format_set_has(&wl->linux_dmabuf_v1_formats,
			dmabuf.format, dmabuf.modifier);
	} else if (wlr_buffer_get_shm(wlr_buffer, &shm)) {
		return wlr_drm_format_set_has(&wl->shm_formats, shm.format,
			DRM_FORMAT_MOD_INVALID);
	} else {
		return false;
	}
}

static struct wl_buffer *import_dmabuf(struct wlr_wl_backend *wl,
		struct wlr_dmabuf_attributes *dmabuf) {
	uint32_t modifier_hi = dmabuf->modifier >> 32;
	uint32_t modifier_lo = (uint32_t)dmabuf->modifier;
	struct zwp_linux_buffer_params_v1 *params =
		zwp_linux_dmabuf_v1_create_params(wl->zwp_linux_dmabuf_v1);
	for (int i = 0; i < dmabuf->n_planes; i++) {
		zwp_linux_buffer_params_v1_add(params, dmabuf->fd[i], i,
			dmabuf->offset[i], dmabuf->stride[i], modifier_hi, modifier_lo);
	}

	struct wl_buffer *wl_buffer = zwp_linux_buffer_params_v1_create_immed(
		params, dmabuf->width, dmabuf->height, dmabuf->format, 0);
	// TODO: handle create() errors
	return wl_buffer;
}

static struct wl_buffer *import_shm(struct wlr_wl_backend *wl,
		struct wlr_shm_attributes *shm) {
	enum wl_shm_format wl_shm_format = convert_drm_format_to_wl_shm(shm->format);
	uint32_t size = shm->stride * shm->height;
	struct wl_shm_pool *pool = wl_shm_create_pool(wl->shm, shm->fd, size);
	if (pool == NULL) {
		return NULL;
	}
	struct wl_buffer *wl_buffer = wl_shm_pool_create_buffer(pool, shm->offset,
		shm->width, shm->height, shm->stride, wl_shm_format);
	wl_shm_pool_destroy(pool);
	return wl_buffer;
}

static struct wlr_wl_buffer *create_wl_buffer(struct wlr_wl_backend *wl,
		struct wlr_buffer *wlr_buffer) {
	if (!test_buffer(wl, wlr_buffer)) {
		return NULL;
	}

	struct wlr_dmabuf_attributes dmabuf;
	struct wlr_shm_attributes shm;
	struct wl_buffer *wl_buffer;
	if (wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		wl_buffer = import_dmabuf(wl, &dmabuf);
	} else if (wlr_buffer_get_shm(wlr_buffer, &shm)) {
		wl_buffer = import_shm(wl, &shm);
	} else {
		return NULL;
	}
	if (wl_buffer == NULL) {
		return NULL;
	}

	struct wlr_wl_buffer *buffer = calloc(1, sizeof(struct wlr_wl_buffer));
	if (buffer == NULL) {
		wl_buffer_destroy(wl_buffer);
		return NULL;
	}
	buffer->wl_buffer = wl_buffer;
	buffer->buffer = wlr_buffer_lock(wlr_buffer);
	wl_list_insert(&wl->buffers, &buffer->link);

	wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);

	buffer->buffer_destroy.notify = buffer_handle_buffer_destroy;
	wl_signal_add(&wlr_buffer->events.destroy, &buffer->buffer_destroy);

	return buffer;
}

static struct wlr_wl_buffer *get_or_create_wl_buffer(struct wlr_wl_backend *wl,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_wl_buffer *buffer;
	wl_list_for_each(buffer, &wl->buffers, link) {
		// We can only re-use a wlr_wl_buffer if the parent compositor has
		// released it, because wl_buffer.release is per-wl_buffer, not per
		// wl_surface.commit.
		if (buffer->buffer == wlr_buffer && buffer->released) {
			buffer->released = false;
			wlr_buffer_lock(buffer->buffer);
			return buffer;
		}
	}

	return create_wl_buffer(wl, wlr_buffer);
}

static bool output_test(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_wl_output *output =
		get_wl_output_from_output(wlr_output);

	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}

	// Adaptive sync is effectively always enabled when using the Wayland
	// backend. This is not something we have control over, so we set the state
	// to enabled on creating the output and never allow changing it.
	assert(wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	if (state->committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) {
		if (!state->adaptive_sync_enabled) {
			return false;
		}
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
	}

	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
			!test_buffer(output->backend, state->buffer)) {
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_LAYERS) {
		// If we can't use a sub-surface for a layer, then we can't use a
		// sub-surface for any layer underneath
		bool supported = output->backend->subcompositor != NULL;
		for (ssize_t i = state->layers_len - 1; i >= 0; i--) {
			struct wlr_output_layer_state *layer_state = &state->layers[i];
			if (layer_state->buffer != NULL) {
				if (layer_state->x < 0 || layer_state->y < 0 ||
						layer_state->x + layer_state->buffer->width > wlr_output->width ||
						layer_state->y + layer_state->buffer->height > wlr_output->height) {
					supported = false;
				}
				supported = supported &&
					test_buffer(output->backend, layer_state->buffer);
			}
			layer_state->accepted = supported;
		}
	}

	return true;
}

static void output_layer_handle_addon_destroy(struct wlr_addon *addon) {
	struct wlr_wl_output_layer *layer = wl_container_of(addon, layer, addon);

	wlr_addon_finish(&layer->addon);
	wl_subsurface_destroy(layer->subsurface);
	wl_surface_destroy(layer->surface);
	free(layer);
}

static const struct wlr_addon_interface output_layer_addon_impl = {
	.name = "wlr_wl_output_layer",
	.destroy = output_layer_handle_addon_destroy,
};

static struct wlr_wl_output_layer *get_or_create_output_layer(
		struct wlr_wl_output *output, struct wlr_output_layer *wlr_layer) {
	assert(output->backend->subcompositor != NULL);

	struct wlr_wl_output_layer *layer;
	struct wlr_addon *addon = wlr_addon_find(&wlr_layer->addons, output,
		&output_layer_addon_impl);
	if (addon != NULL) {
		layer = wl_container_of(addon, layer, addon);
		return layer;
	}

	layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		return NULL;
	}

	wlr_addon_init(&layer->addon, &wlr_layer->addons, output,
		&output_layer_addon_impl);

	layer->surface = wl_compositor_create_surface(output->backend->compositor);
	layer->subsurface = wl_subcompositor_get_subsurface(
		output->backend->subcompositor, layer->surface, output->surface);

	// Set an empty input region so that input events are handled by the main
	// surface
	struct wl_region *region = wl_compositor_create_region(output->backend->compositor);
	wl_surface_set_input_region(layer->surface, region);
	wl_region_destroy(region);

	return layer;
}

static bool output_layer_commit(struct wlr_wl_output *output,
		struct wlr_wl_output_layer *layer,
		const struct wlr_output_layer_state *state) {
	// TODO: only do this if the layer moved
	wl_subsurface_set_position(layer->subsurface, state->x, state->y);

	struct wlr_wl_buffer *buffer = NULL;
	if (state->buffer != NULL) {
		buffer = get_or_create_wl_buffer(output->backend, state->buffer);
		if (buffer == NULL) {
			return false;
		}
	}

	wl_surface_attach(layer->surface, buffer ? buffer->wl_buffer : NULL, 0, 0);
	wl_surface_damage_buffer(layer->surface, 0, 0, INT32_MAX, INT32_MAX);

	wl_surface_commit(layer->surface);
	return true;
}

static bool commit_layers(struct wlr_wl_output *output,
		struct wlr_output_layer_state *layers, size_t layers_len) {
	if (output->backend->subcompositor == NULL) {
		return true;
	}

	struct wlr_wl_output_layer *prev_layer = NULL;
	for (size_t i = 0; i < layers_len; i++) {
		struct wlr_wl_output_layer *layer =
			get_or_create_output_layer(output, layers[i].layer);
		if (layer == NULL) {
			return false;
		}

		if (!layers[i].accepted) {
			// Unmap the sub-surface
			// TODO: only do this once
			wl_surface_attach(layer->surface, NULL, 0, 0);
			wl_surface_commit(layer->surface);
			continue;
		}

		// TODO: only do this if layers were re-ordered
		if (prev_layer != NULL) {
			wl_subsurface_place_above(layer->subsurface,
				prev_layer->surface);
		}

		if (!output_layer_commit(output, layer, &layers[i])) {
			return false;
		}
	}

	// Unmap any layer we haven't seen
	struct wlr_output_layer *wlr_layer;
	wl_list_for_each(wlr_layer, &output->wlr_output.layers, link) {
		bool found = false;
		for (size_t i = 0; i < layers_len; i++) {
			if (layers[i].layer == wlr_layer) {
				found = true;
				break;
			}
		}
		if (found) {
			continue;
		}

		struct wlr_wl_output_layer *layer =
			get_or_create_output_layer(output, wlr_layer);
		if (layer == NULL) {
			continue;
		}

		// TODO: only do this once
		wl_surface_attach(layer->surface, NULL, 0, 0);
		wl_surface_commit(layer->surface);
	}

	return true;
}

static bool output_commit(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_wl_output *output =
		get_wl_output_from_output(wlr_output);

	if (!output_test(wlr_output, state)) {
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		const pixman_region32_t *damage = NULL;
		if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
			damage = &state->damage;
		}

		struct wlr_buffer *wlr_buffer = state->buffer;
		struct wlr_wl_buffer *buffer =
			get_or_create_wl_buffer(output->backend, wlr_buffer);
		if (buffer == NULL) {
			return false;
		}

		wl_surface_attach(output->surface, buffer->wl_buffer, 0, 0);

		if (damage == NULL) {
			wl_surface_damage_buffer(output->surface,
				0, 0, INT32_MAX, INT32_MAX);
		} else {
			int rects_len;
			const pixman_box32_t *rects =
				pixman_region32_rectangles(damage, &rects_len);
			for (int i = 0; i < rects_len; i++) {
				const pixman_box32_t *r = &rects[i];
				wl_surface_damage_buffer(output->surface, r->x1, r->y1,
					r->x2 - r->x1, r->y2 - r->y1);
			}
		}
	}

	if ((state->committed & WLR_OUTPUT_STATE_LAYERS) &&
			!commit_layers(output, state->layers, state->layers_len)) {
		return false;
	}

	if (state->committed & (WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_LAYERS)) {
		if (output->frame_callback != NULL) {
			wl_callback_destroy(output->frame_callback);
		}
		output->frame_callback = wl_surface_frame(output->surface);
		wl_callback_add_listener(output->frame_callback, &frame_listener, output);

		struct wp_presentation_feedback *wp_feedback = NULL;
		if (output->backend->presentation != NULL) {
			wp_feedback = wp_presentation_feedback(output->backend->presentation,
				output->surface);
		}

		wl_surface_commit(output->surface);

		if (wp_feedback != NULL) {
			struct wlr_wl_presentation_feedback *feedback =
				calloc(1, sizeof(*feedback));
			if (feedback == NULL) {
				wp_presentation_feedback_destroy(wp_feedback);
				return false;
			}
			feedback->output = output;
			feedback->feedback = wp_feedback;
			feedback->commit_seq = output->wlr_output.commit_seq + 1;
			wl_list_insert(&output->presentation_feedbacks, &feedback->link);

			wp_presentation_feedback_add_listener(wp_feedback,
				&presentation_feedback_listener, feedback);
		} else {
			struct wlr_output_event_present present_event = {
				.commit_seq = wlr_output->commit_seq + 1,
				.presented = true,
			};
			wlr_output_send_present(wlr_output, &present_event);
		}
	}

	wl_display_flush(output->backend->remote_display);

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		wlr_output_update_custom_mode(wlr_output,
			state->custom_mode.width, state->custom_mode.height, 0);
	}

	return true;
}

static bool output_set_cursor(struct wlr_output *wlr_output,
		struct wlr_buffer *wlr_buffer, int hotspot_x, int hotspot_y) {
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);
	struct wlr_wl_backend *backend = output->backend;

	output->cursor.hotspot_x = hotspot_x;
	output->cursor.hotspot_y = hotspot_y;

	if (output->cursor.surface == NULL) {
		output->cursor.surface =
			wl_compositor_create_surface(backend->compositor);
	}
	struct wl_surface *surface = output->cursor.surface;

	if (wlr_buffer != NULL) {
		struct wlr_wl_buffer *buffer =
			get_or_create_wl_buffer(output->backend, wlr_buffer);
		if (buffer == NULL) {
			return false;
		}

		wl_surface_attach(surface, buffer->wl_buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_commit(surface);
	} else {
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
	}

	update_wl_output_cursor(output);
	wl_display_flush(backend->remote_display);
	return true;
}

static const struct wlr_drm_format_set *output_get_formats(
		struct wlr_output *wlr_output, uint32_t buffer_caps) {
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);
	if (buffer_caps & WLR_BUFFER_CAP_DMABUF) {
		return &output->backend->linux_dmabuf_v1_formats;
	} else if (buffer_caps & WLR_BUFFER_CAP_SHM) {
		return &output->backend->shm_formats;
	}
	return NULL;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);
	if (output == NULL) {
		return;
	}

	wl_list_remove(&output->link);

	if (output->cursor.surface) {
		wl_surface_destroy(output->cursor.surface);
	}

	if (output->frame_callback) {
		wl_callback_destroy(output->frame_callback);
	}

	struct wlr_wl_presentation_feedback *feedback, *feedback_tmp;
	wl_list_for_each_safe(feedback, feedback_tmp,
			&output->presentation_feedbacks, link) {
		presentation_feedback_destroy(feedback);
	}

	if (output->zxdg_toplevel_decoration_v1) {
		zxdg_toplevel_decoration_v1_destroy(output->zxdg_toplevel_decoration_v1);
	}
	xdg_toplevel_destroy(output->xdg_toplevel);
	xdg_surface_destroy(output->xdg_surface);
	wl_surface_destroy(output->surface);
	wl_display_flush(output->backend->remote_display);
	free(output);
}

void update_wl_output_cursor(struct wlr_wl_output *output) {
	struct wlr_wl_pointer *pointer = output->cursor.pointer;
	if (pointer) {
		assert(pointer->output == output);
		assert(output->enter_serial);

		struct wlr_wl_seat *seat = pointer->seat;
		wl_pointer_set_cursor(seat->wl_pointer, output->enter_serial,
			output->cursor.surface, output->cursor.hotspot_x,
			output->cursor.hotspot_y);
	}
}

static bool output_move_cursor(struct wlr_output *_output, int x, int y) {
	// TODO: only return true if x == current x and y == current y
	return true;
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
	.set_cursor = output_set_cursor,
	.move_cursor = output_move_cursor,
	.get_cursor_formats = output_get_formats,
	.get_primary_formats = output_get_formats,
};

bool wlr_output_is_wl(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static void xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial) {
	struct wlr_wl_output *output = data;
	assert(output && output->xdg_surface == xdg_surface);

	xdg_surface_ack_configure(xdg_surface, serial);

	// nothing else?
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data,
		struct xdg_toplevel *xdg_toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	struct wlr_wl_output *output = data;
	assert(output && output->xdg_toplevel == xdg_toplevel);

	if (width == 0 || height == 0) {
		return;
	}

	struct wlr_output_state state = {
		.committed = WLR_OUTPUT_STATE_MODE,
		.mode_type = WLR_OUTPUT_STATE_MODE_CUSTOM,
		.custom_mode = { .width = width, .height = height },
	};
	wlr_output_send_request_state(&output->wlr_output, &state);
}

static void xdg_toplevel_handle_close(void *data,
		struct xdg_toplevel *xdg_toplevel) {
	struct wlr_wl_output *output = data;
	assert(output && output->xdg_toplevel == xdg_toplevel);

	wlr_output_destroy(&output->wlr_output);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

struct wlr_output *wlr_wl_output_create(struct wlr_backend *wlr_backend) {
	struct wlr_wl_backend *backend = get_wl_backend_from_backend(wlr_backend);
	if (!backend->started) {
		++backend->requested_outputs;
		return NULL;
	}

	struct wlr_wl_output *output;
	if (!(output = calloc(sizeof(struct wlr_wl_output), 1))) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_wl_output");
		return NULL;
	}
	wlr_output_init(&output->wlr_output, &backend->backend, &output_impl,
		backend->local_display);
	struct wlr_output *wlr_output = &output->wlr_output;

	wlr_output_update_custom_mode(wlr_output, 1280, 720, 0);

	wlr_output->adaptive_sync_status = WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;

	size_t output_num = ++last_output_num;

	char name[64];
	snprintf(name, sizeof(name), "WL-%zu", output_num);
	wlr_output_set_name(wlr_output, name);

	char description[128];
	snprintf(description, sizeof(description), "Wayland output %zu", output_num);
	wlr_output_set_description(wlr_output, description);

	output->backend = backend;
	wl_list_init(&output->presentation_feedbacks);

	output->surface = wl_compositor_create_surface(backend->compositor);
	if (!output->surface) {
		wlr_log_errno(WLR_ERROR, "Could not create output surface");
		goto error;
	}
	wl_surface_set_user_data(output->surface, output);
	output->xdg_surface =
		xdg_wm_base_get_xdg_surface(backend->xdg_wm_base, output->surface);
	if (!output->xdg_surface) {
		wlr_log_errno(WLR_ERROR, "Could not get xdg surface");
		goto error;
	}
	output->xdg_toplevel =
		xdg_surface_get_toplevel(output->xdg_surface);
	if (!output->xdg_toplevel) {
		wlr_log_errno(WLR_ERROR, "Could not get xdg toplevel");
		goto error;
	}

	if (backend->zxdg_decoration_manager_v1) {
		output->zxdg_toplevel_decoration_v1 =
			zxdg_decoration_manager_v1_get_toplevel_decoration(
			backend->zxdg_decoration_manager_v1, output->xdg_toplevel);
		if (!output->zxdg_toplevel_decoration_v1) {
			wlr_log_errno(WLR_ERROR, "Could not get xdg toplevel decoration");
			goto error;
		}
		zxdg_toplevel_decoration_v1_set_mode(output->zxdg_toplevel_decoration_v1,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}

	wlr_wl_output_set_title(wlr_output, NULL);

	xdg_toplevel_set_app_id(output->xdg_toplevel, "wlroots");
	xdg_surface_add_listener(output->xdg_surface,
			&xdg_surface_listener, output);
	xdg_toplevel_add_listener(output->xdg_toplevel,
			&xdg_toplevel_listener, output);
	wl_surface_commit(output->surface);

	wl_display_roundtrip(output->backend->remote_display);

	wl_list_insert(&backend->outputs, &output->link);
	wlr_output_update_enabled(wlr_output, true);

	wl_signal_emit_mutable(&backend->backend.events.new_output, wlr_output);

	struct wlr_wl_seat *seat;
	wl_list_for_each(seat, &backend->seats, link) {
		if (seat->wl_pointer) {
			create_pointer(seat, output);
		}
	}

	// TODO: let the compositor do this bit
	if (backend->activation_v1 && backend->activation_token) {
		xdg_activation_v1_activate(backend->activation_v1,
				backend->activation_token, output->surface);
	}

	// Start the rendering loop by requesting the compositor to render a frame
	wlr_output_schedule_frame(wlr_output);

	return wlr_output;

error:
	wlr_output_destroy(&output->wlr_output);
	return NULL;
}

void wlr_wl_output_set_title(struct wlr_output *output, const char *title) {
	struct wlr_wl_output *wl_output = get_wl_output_from_output(output);

	char wl_title[32];
	if (title == NULL) {
		if (snprintf(wl_title, sizeof(wl_title), "wlroots - %s", output->name) <= 0) {
			return;
		}
		title = wl_title;
	}

	xdg_toplevel_set_title(wl_output->xdg_toplevel, title);
	wl_display_flush(wl_output->backend->remote_display);
}

struct wl_surface *wlr_wl_output_get_surface(struct wlr_output *output) {
	struct wlr_wl_output *wl_output = get_wl_output_from_output(output);
	return wl_output->surface;
}
