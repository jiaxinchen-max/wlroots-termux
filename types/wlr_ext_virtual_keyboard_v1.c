#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_ext_virtual_keyboard_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "ext-virtual-keyboard-v1-protocol.h"

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "ext-virtual-keyboard",
};

static const struct ext_virtual_keyboard_v1_interface ext_virtual_keyboard_impl;

static struct wlr_ext_virtual_keyboard_v1 *ext_virtual_keyboard_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_virtual_keyboard_v1_interface, &ext_virtual_keyboard_impl));
	return wl_resource_get_user_data(resource);
}

struct wlr_ext_virtual_keyboard_v1 *wlr_ext_virtual_keyboard_v1_try_from_wlr_input_device(
		struct wlr_input_device *wlr_dev) {
	if (wlr_dev->type != WLR_INPUT_DEVICE_KEYBOARD) {
		return NULL;
	}
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(wlr_dev);
	if (wlr_keyboard->impl != &keyboard_impl) {
		return NULL;
	}
	return wl_container_of(wlr_keyboard,
		(struct wlr_ext_virtual_keyboard_v1 *)NULL, keyboard);
}

static void ext_virtual_keyboard_keymap(struct wl_client *client,
		struct wl_resource *resource, uint32_t format, int32_t fd,
		uint32_t size) {
	struct wlr_ext_virtual_keyboard_v1 *keyboard =
		ext_virtual_keyboard_from_resource(resource);
	if (keyboard == NULL) {
		return;
	}

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wl_client_post_no_memory(client);
		goto context_fail;
	}
	void *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED) {
		wl_resource_post_error(resource,
			EXT_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP,
			"Failed to map file descriptor to memory");
		goto fd_fail;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(context, data,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(data, size);
	if (!keymap) {
		wl_resource_post_error(resource,
			EXT_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP,
			"Failed to parse keymap");
		goto keymap_fail;
	}
	wlr_keyboard_set_keymap(&keyboard->keyboard, keymap);
	keyboard->has_keymap = true;
	xkb_keymap_unref(keymap);
keymap_fail:
fd_fail:
	xkb_context_unref(context);
context_fail:
	close(fd);
}

static void ext_virtual_keyboard_key(struct wl_client *client,
		struct wl_resource *resource, uint32_t time, uint32_t key,
		uint32_t state) {
	struct wlr_ext_virtual_keyboard_v1 *keyboard =
		ext_virtual_keyboard_from_resource(resource);
	if (keyboard == NULL) {
		return;
	}
	if (!keyboard->has_keymap) {
		wl_resource_post_error(resource,
			EXT_VIRTUAL_KEYBOARD_V1_ERROR_MISSING_KEYMAP,
			"Cannot send a keypress before defining a keymap");
		return;
	}
	struct wlr_keyboard_key_event event = {
		.time_msec = time,
		.keycode = key,
		.update_state = false,
		.state = state,
	};
	wlr_keyboard_notify_key(&keyboard->keyboard, &event);
}

static void ext_virtual_keyboard_modifiers(struct wl_client *client,
		struct wl_resource *resource, uint32_t mods_depressed,
		uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
	struct wlr_ext_virtual_keyboard_v1 *keyboard =
		ext_virtual_keyboard_from_resource(resource);
	if (keyboard == NULL) {
		return;
	}
	if (!keyboard->has_keymap) {
		wl_resource_post_error(resource,
			EXT_VIRTUAL_KEYBOARD_V1_ERROR_MISSING_KEYMAP,
			"Cannot send a modifier state before defining a keymap");
		return;
	}
	wlr_keyboard_notify_modifiers(&keyboard->keyboard,
		mods_depressed, mods_latched, mods_locked, group);
}

static void ext_virtual_keyboard_repeat_info(struct wl_client *client,
		struct wl_resource *resource, int32_t rate, int32_t delay) {
	struct wlr_ext_virtual_keyboard_v1 *keyboard =
		ext_virtual_keyboard_from_resource(resource);
	if (keyboard == NULL) {
		return;
	}

	wlr_keyboard_set_repeat_info(&keyboard->keyboard, rate, delay);
}

static void ext_virtual_keyboard_destroy_resource(struct wl_resource *resource) {
	struct wlr_ext_virtual_keyboard_v1 *keyboard =
		ext_virtual_keyboard_from_resource(resource);
	if (keyboard == NULL) {
		return;
	}

	wlr_keyboard_finish(&keyboard->keyboard);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void ext_virtual_keyboard_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_virtual_keyboard_v1_interface ext_virtual_keyboard_impl = {
	.keymap = ext_virtual_keyboard_keymap,
	.key = ext_virtual_keyboard_key,
	.modifiers = ext_virtual_keyboard_modifiers,
	.repeat_info = ext_virtual_keyboard_repeat_info,
	.destroy = ext_virtual_keyboard_destroy,
};

static const struct ext_virtual_keyboard_manager_v1_interface manager_impl;

static struct wlr_ext_virtual_keyboard_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_virtual_keyboard_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void ext_virtual_keyboard_manager_create_ext_virtual_keyboard(
		struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat, uint32_t id) {
	struct wlr_ext_virtual_keyboard_manager_v1 *manager =
		manager_from_resource(resource);
	struct wlr_seat_client *seat_client = wlr_seat_client_from_resource(seat);

	struct wl_resource *keyboard_resource = wl_resource_create(client,
		&ext_virtual_keyboard_v1_interface, wl_resource_get_version(resource),
		id);
	if (!keyboard_resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(keyboard_resource, &ext_virtual_keyboard_impl,
		NULL, ext_virtual_keyboard_destroy_resource);
	if (seat_client == NULL) {
		return;
	}

	struct wlr_ext_virtual_keyboard_v1 *ext_virtual_keyboard = calloc(1, sizeof(*ext_virtual_keyboard));
	if (!ext_virtual_keyboard) {
		wl_client_post_no_memory(client);
		return;
	}

	wlr_keyboard_init(&ext_virtual_keyboard->keyboard, &keyboard_impl,
		"wlr_ext_virtual_keyboard_v1");

	ext_virtual_keyboard->resource = keyboard_resource;
	ext_virtual_keyboard->seat = seat_client->seat;
	wl_resource_set_user_data(keyboard_resource, ext_virtual_keyboard);

	wl_list_insert(&manager->keyboards, &ext_virtual_keyboard->link);

	wl_signal_emit_mutable(&manager->events.new_keyboard,
		ext_virtual_keyboard);
}

static const struct ext_virtual_keyboard_manager_v1_interface manager_impl = {
	.create_virtual_keyboard = ext_virtual_keyboard_manager_create_ext_virtual_keyboard,
};

static void ext_virtual_keyboard_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_ext_virtual_keyboard_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&ext_virtual_keyboard_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_ext_virtual_keyboard_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, manager);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_ext_virtual_keyboard_manager_v1 *wlr_ext_virtual_keyboard_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	struct wlr_ext_virtual_keyboard_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&ext_virtual_keyboard_manager_v1_interface, version, manager,
		ext_virtual_keyboard_manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	wl_list_init(&manager->keyboards);

	wl_signal_init(&manager->events.new_keyboard);
	wl_signal_init(&manager->events.destroy);

	return manager;
}
