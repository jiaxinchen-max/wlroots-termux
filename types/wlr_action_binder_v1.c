#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_action_binder_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <util/time.h>
#include "ext-action-binder-v1-protocol.h"

static void resource_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_action_binder_v1_interface ext_action_binder_v1_implementation;
static struct wlr_action_binder_v1_state *wlr_action_binder_v1_state_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &ext_action_binder_v1_interface, &ext_action_binder_v1_implementation));
	return wl_resource_get_user_data(resource);
}

static const struct ext_action_binding_v1_interface ext_action_binding_v1_implementation;
static struct wlr_action_binding_v1 *wlr_action_binding_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &ext_action_binding_v1_interface, &ext_action_binding_v1_implementation));
	return wl_resource_get_user_data(resource);
}

static void destroy_binding(struct wlr_action_binding_v1 *binding) {
	if (!binding) {
		return;
	}

	free(binding->category);
	free(binding->name);

	if (binding->hint.type == WLR_ACTION_BINDING_HINT_V1_KEYBOARD) {
		free(binding->hint.keycombo);
	}

	free(binding->description);

	free(binding->app_id);

	wl_list_remove(&binding->link);
	wl_list_remove(&binding->seat_destroy.link);

	wl_resource_set_user_data(binding->resource, NULL); // make resource inert

	free(binding);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct wlr_action_binding_v1 *binding = wl_container_of(listener, binding, seat_destroy);
	wl_list_remove(&binding->seat_destroy.link);
	binding->seat = NULL;
	wlr_action_binding_v1_reject(binding);
}

static void action_binding_destroy(struct wl_resource *resource) {
	struct wlr_action_binding_v1 *binding = wlr_action_binding_v1_from_resource(resource);
	destroy_binding(binding);
}

static void action_binding_set_name(struct wl_client *client,
		struct wl_resource *resource, const char *category, const char *name) {
	struct wlr_action_binding_v1 *binding = wlr_action_binding_v1_from_resource(resource);
	if (!binding) {
		return;
	}

	if (binding->bound || binding->name || binding->category) {
		wl_resource_post_error(binding->resource,
			EXT_ACTION_BINDING_V1_ERROR_ALREADY_SET,
			"attempted to set a binding property twice");
		return;
	}

	binding->name = strdup(name);
	if (binding->name == NULL) {
		wl_client_post_no_memory(client);
	}

	binding->category = strdup(category);
	if (binding->category == NULL) {
		wl_client_post_no_memory(client);
	}
}

static struct wlr_action_binding_hint_v1 *action_binding_hint_from_resource(struct wl_resource *resource) {
	struct wlr_action_binding_v1 *binding = wlr_action_binding_v1_from_resource(resource);
	if (!binding) {
		return NULL;
	}
	if (binding->bound || binding->hint.type != WLR_ACTION_BINDING_HINT_V1_NONE) {
		wl_resource_post_error(binding->resource,
			EXT_ACTION_BINDING_V1_ERROR_ALREADY_SET, "attempted to set a hint twice");
		return NULL;
	}

	return &binding->hint;
}

static void action_binding_set_mouse_hint(struct wl_client *client,
		struct wl_resource *resource, uint32_t button) {
	struct wlr_action_binding_hint_v1 *hint = action_binding_hint_from_resource(resource);
	if (!hint) {
		return;
	}

	hint->type = WLR_ACTION_BINDING_HINT_V1_MOUSE;
	hint->mouse_button = button;
}

static void action_binding_set_keyboard_hint(struct wl_client *client,
		struct wl_resource *resource, const char *keycombo) {
	struct wlr_action_binding_hint_v1 *hint = action_binding_hint_from_resource(resource);
	if (!hint) {
		return;
	}

	hint->type = WLR_ACTION_BINDING_HINT_V1_KEYBOARD;
	hint->keycombo = strdup(keycombo);
}

static void action_binding_set_gesture_hint(struct wl_client *client,
		struct wl_resource *resource, uint32_t type, uint32_t direction, uint32_t fingers) {
	struct wlr_action_binding_hint_v1 *hint = action_binding_hint_from_resource(resource);
	if (!hint) {
		return;
	}

	hint->type = WLR_ACTION_BINDING_HINT_V1_GESTURE;
	hint->gesture.type = type;
	hint->gesture.direction = direction;
	hint->gesture.fingers = fingers;
}

static void action_binding_set_desc(struct wl_client *client,
		struct wl_resource *resource, const char *description) {
	struct wlr_action_binding_v1 *binding = wlr_action_binding_v1_from_resource(resource);
	if (!binding) {
		return;
	}
	if (binding->bound || binding->description) {
		wl_resource_post_error(binding->resource,
			EXT_ACTION_BINDING_V1_ERROR_ALREADY_SET,
			"attempted to set a binding property twice");
		return;
	}

	binding->description = strdup(description);
	if (binding->description == NULL) {
		wl_client_post_no_memory(client);
	}
}

static void action_binding_set_app_id(struct wl_client *client,
		struct wl_resource *resource, const char *app_id) {
	struct wlr_action_binding_v1 *binding = wlr_action_binding_v1_from_resource(resource);
	if (!binding) {
		return;
	}
	if (binding->bound || binding->app_id) {
		wl_resource_post_error(binding->resource,
			EXT_ACTION_BINDING_V1_ERROR_ALREADY_SET,
			"attempted to set a binding property twice");
		return;
	}

	binding->app_id = strdup(app_id);
	if (binding->app_id == NULL) {
		wl_client_post_no_memory(client);
	}
}

static void action_binding_set_seat(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *seat_resource) {
	struct wlr_action_binding_v1 *binding = wlr_action_binding_v1_from_resource(resource);
	if (!binding) {
		return;
	}

	if (binding->bound || binding->seat) {
		wl_resource_post_error(binding->resource,
			EXT_ACTION_BINDING_V1_ERROR_ALREADY_SET,
			"attempted to set a binding property twice");
		return;
	}

	struct wlr_seat_client *seat_client = wlr_seat_client_from_resource(seat_resource);
	binding->seat = seat_client ? seat_client->seat : NULL;
	if (binding->seat) {
		wl_signal_add(&binding->seat->events.destroy, &binding->seat_destroy);
	}
}

static const struct ext_action_binding_v1_interface ext_action_binding_v1_implementation = {
	.destroy = resource_handle_destroy,
	.set_mouse_hint = action_binding_set_mouse_hint,
	.set_keyboard_hint = action_binding_set_keyboard_hint,
	.set_gesture_hint = action_binding_set_gesture_hint,
	.set_description = action_binding_set_desc,
	.set_name = action_binding_set_name,
	.set_app_id = action_binding_set_app_id,
	.set_seat = action_binding_set_seat,
};

static void action_binder_create_binding(struct wl_client *client,
		struct wl_resource *resource, uint32_t binding) {
	struct wlr_action_binder_v1_state *state = wlr_action_binder_v1_state_from_resource(resource);

	struct wl_resource *bind_resource = wl_resource_create(client,
		&ext_action_binding_v1_interface, ext_action_binding_v1_interface.version, binding);
	if (bind_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct wlr_action_binding_v1 *bind = calloc(1, sizeof(*bind));
	if (bind == NULL) {
		wl_client_post_no_memory(client);
		wl_resource_destroy(bind_resource);
		return;
	}
	bind->resource = bind_resource;
	bind->state = state;
	bind->bound = false;

	wl_list_init(&bind->link);
	wl_signal_init(&bind->events.destroy);
	wl_list_insert(&state->bind_queue, &bind->link);

	wl_list_init(&bind->seat_destroy.link);
	bind->seat_destroy.notify = handle_seat_destroy;

	wl_resource_set_implementation(bind_resource,
		&ext_action_binding_v1_implementation, bind, action_binding_destroy);
}

static void action_binder_commit(struct wl_client *client, struct wl_resource *resource) {
	struct wlr_action_binder_v1_state *state = wlr_action_binder_v1_state_from_resource(resource);
	struct wlr_action_binding_v1 *binding = NULL;

	wl_list_for_each(binding, &state->bind_queue, link) {
		if (!binding->category || !binding->name) {
			wl_resource_post_error(binding->resource,
				EXT_ACTION_BINDER_V1_ERROR_INVALID_BINDING,
				"attempted to bind a unactionable binding");
			return;
		}
	}

	wl_signal_emit_mutable(&state->binder->events.bind, state);
}

static const struct ext_action_binder_v1_interface ext_action_binder_v1_implementation = {
	.create_binding = action_binder_create_binding,
	.commit = action_binder_commit,
	.destroy = resource_handle_destroy,
};

static void action_binder_destroy(struct wl_resource *resource) {
	struct wlr_action_binder_v1_state *state = wlr_action_binder_v1_state_from_resource(resource);
	struct wlr_action_binding_v1 *binding = NULL, *tmp = NULL;

	wl_list_for_each_safe(binding, tmp, &state->binds, link) {
		wl_signal_emit(&binding->events.destroy, NULL);
		destroy_binding(binding);
	}

	wl_list_for_each_safe(binding, tmp, &state->bind_queue, link) {
		destroy_binding(binding);
	}

	wl_list_remove(&state->link);

	free(state);
}

static void action_binder_bind(struct wl_client *wl_client,
		void *data, uint32_t version, uint32_t id) {
	struct wlr_action_binder_v1 *binder = data;

	struct wl_resource *resource = wl_resource_create(wl_client,
		&ext_action_binder_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	struct wlr_action_binder_v1_state *state = calloc(1, sizeof(*state));
	if (state == NULL) {
		wl_client_post_no_memory(wl_client);
		wl_resource_destroy(resource);
		return;
	}

	wl_list_init(&state->binds);
	wl_list_init(&state->bind_queue);
	state->binder = binder;
	state->resource = resource;

	wl_list_insert(&binder->states, &state->link);

	wl_resource_set_implementation(resource,
		&ext_action_binder_v1_implementation, state, action_binder_destroy);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_action_binder_v1 *binder = wl_container_of(listener, binder, display_destroy);
	wl_signal_emit(&binder->events.destroy, NULL);
	wl_list_remove(&binder->display_destroy.link);
	wl_global_destroy(binder->global);
	free(binder);
}

struct wlr_action_binder_v1 *wlr_action_binder_v1_create(struct wl_display *display) {
	struct wlr_action_binder_v1 *action_binder = calloc(1, sizeof(*action_binder));
	if (!action_binder) {
		return NULL;
	}

	struct wl_global *global = wl_global_create(display,
		&ext_action_binder_v1_interface, 1, action_binder, action_binder_bind);
	if (!global) {
		free(action_binder);
		return NULL;
	}
	action_binder->global = global;

	wl_signal_init(&action_binder->events.bind);
	wl_signal_init(&action_binder->events.destroy);
	wl_list_init(&action_binder->states);

	action_binder->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &action_binder->display_destroy);

	return action_binder;
}

void wlr_action_binding_v1_bind(struct wlr_action_binding_v1 *binding, const char *trigger) {
	assert(!binding->bound);
	binding->bound = true;
	wl_list_remove(&binding->link);
	wl_list_insert(&binding->state->binds, &binding->link);

	ext_action_binding_v1_send_bound(binding->resource, trigger);
}

void wlr_action_binding_v1_reject(struct wlr_action_binding_v1 *binding) {
	ext_action_binding_v1_send_rejected(binding->resource);
	wl_signal_emit(&binding->events.destroy, NULL);
	destroy_binding(binding);
}

void wlr_action_binding_v1_trigger(struct wlr_action_binding_v1 *binding, uint32_t trigger_type, uint32_t time_msec) {
	ext_action_binding_v1_send_triggered(binding->resource, time_msec, trigger_type);
}
