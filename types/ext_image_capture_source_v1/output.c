#include <assert.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>
#include "render/wlr_renderer.h"
#include "ext-image-capture-source-v1-protocol.h"

#define OUTPUT_IMAGE_SOURCE_MANAGER_V1_VERSION 1

struct wlr_ext_output_image_capture_source_v1 {
	struct wlr_ext_image_capture_source_v1 base;
	struct wlr_addon addon;

	struct wlr_output *output;

	struct wl_listener output_commit;
};

struct wlr_ext_output_image_capture_source_v1_frame_event {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec *when;
};

static void output_source_schedule_frame(struct wlr_ext_image_capture_source_v1 *base) {
	struct wlr_ext_output_image_capture_source_v1 *source = wl_container_of(base, source, base);
	wlr_output_update_needs_frame(source->output);
}

static void output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct wlr_ext_output_image_capture_source_v1 *source = wl_container_of(base, source, base);
	struct wlr_ext_output_image_capture_source_v1_frame_event *event =
		wl_container_of(base_event, event, base);

	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame,
			event->buffer, source->output->renderer)) {
		wlr_ext_image_copy_capture_frame_v1_ready(frame,
			source->output->transform, event->when);
	}
}

static const struct wlr_ext_image_capture_source_v1_interface output_source_impl = {
	.schedule_frame = output_source_schedule_frame,
	.copy_frame = output_source_copy_frame,
};

static void source_update_buffer_constraints(struct wlr_ext_output_image_capture_source_v1 *source) {
	struct wlr_output *output = source->output;

	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return;
	}

	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base,
		output->swapchain, output->renderer);
}

static void source_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct wlr_ext_output_image_capture_source_v1 *source = wl_container_of(listener, source, output_commit);
	struct wlr_output_event_commit *event = data;

	if (event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_RENDER_FORMAT)) {
		source_update_buffer_constraints(source);
	}

	if (event->state->committed & WLR_OUTPUT_STATE_BUFFER) {
		struct wlr_buffer *buffer = event->state->buffer;

		pixman_region32_t full_damage;
		pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

		const pixman_region32_t *damage;
		if (event->state->committed & WLR_OUTPUT_STATE_DAMAGE) {
			damage = &event->state->damage;
		} else {
			damage = &full_damage;
		}

		struct wlr_ext_output_image_capture_source_v1_frame_event frame_event = {
			.base = {
				.damage = damage,
			},
			.buffer = buffer,
			.when = event->when, // TODO: predict next presentation time instead
		};
		wl_signal_emit_mutable(&source->base.events.frame, &frame_event);

		pixman_region32_fini(&full_damage);
	}
}

static void output_addon_destroy(struct wlr_addon *addon) {
	struct wlr_ext_output_image_capture_source_v1 *source = wl_container_of(addon, source, addon);
	wlr_ext_image_capture_source_v1_finish(&source->base);
	wl_list_remove(&source->output_commit.link);
	wlr_addon_finish(&source->addon);
	free(source);
}

static const struct wlr_addon_interface output_addon_impl = {
	.name = "wlr_ext_output_image_capture_source_v1",
	.destroy = output_addon_destroy,
};

static void output_manager_handle_create_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t new_id,
		struct wl_resource *output_resource) {
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	if (output == NULL) {
		wlr_ext_image_capture_source_v1_create_resource(NULL, client, new_id);
		return;
	}

	struct wlr_ext_output_image_capture_source_v1 *source;
	struct wlr_addon *addon = wlr_addon_find(&output->addons, NULL, &output_addon_impl);
	if (addon != NULL) {
		source = wl_container_of(addon, source, addon);
	} else {
		source = calloc(1, sizeof(*source));
		if (source == NULL) {
			wl_resource_post_no_memory(manager_resource);
			return;
		}

		wlr_ext_image_capture_source_v1_init(&source->base, &output_source_impl);
		wlr_addon_init(&source->addon, &output->addons, NULL, &output_addon_impl);
		source->output = output;

		source->output_commit.notify = source_handle_output_commit;
		wl_signal_add(&output->events.commit, &source->output_commit);

		source_update_buffer_constraints(source);
	}

	if (!wlr_ext_image_capture_source_v1_create_resource(&source->base, client, new_id)) {
		return;
	}
}

static void output_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_manager_impl = {
	.create_source = output_manager_handle_create_source,
	.destroy = output_manager_handle_destroy,
};

static void output_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_ext_output_image_capture_source_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &output_manager_impl, manager, NULL);
}

static void output_manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_ext_output_image_capture_source_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_ext_output_image_capture_source_manager_v1 *wlr_ext_output_image_capture_source_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= OUTPUT_IMAGE_SOURCE_MANAGER_V1_VERSION);

	struct wlr_ext_output_image_capture_source_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&ext_output_image_capture_source_manager_v1_interface, version, manager, output_manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = output_manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
