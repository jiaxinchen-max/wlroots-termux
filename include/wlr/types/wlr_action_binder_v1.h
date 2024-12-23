/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_ACTION_BINDER_V1_H
#define WLR_TYPES_WLR_ACTION_BINDER_V1_H

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "ext-action-binder-v1-protocol.h"

struct wlr_action_binder_v1 {
	struct wl_global *global;
	struct wl_list states; // wlr_action_binder_v1_state.link
	struct wl_listener display_destroy;

	struct {
		struct wl_signal bind;
		struct wl_signal destroy;
	} events;

	void *data;
};

struct wlr_action_binder_v1_state {
	struct wl_list binds; // wlr_action_binding_v1.link
	struct wl_list bind_queue; // wlr_action_binding_v1.link
	struct wlr_action_binder_v1 *binder;
	struct wl_resource *resource;

	struct wl_list link;
};

struct wlr_action_binding_hint_v1 {
	enum {
		WLR_ACTION_BINDING_HINT_V1_NONE,
		WLR_ACTION_BINDING_HINT_V1_KEYBOARD,
		WLR_ACTION_BINDING_HINT_V1_MOUSE,
		WLR_ACTION_BINDING_HINT_V1_GESTURE,
	} type;

	union {
		char *keycombo; // WLR_ACTION_BINDING_HINT_V1_KEYBOARD
		uint32_t mouse_button; // WLR_ACTION_BINDING_HINT_V1_MOUSE
		struct {
			enum ext_action_binding_v1_gesture_type type;
			enum ext_action_binding_v1_gesture_direction direction;
			uint32_t fingers;
		} gesture; // WLR_ACTION_BINDING_HINT_V1_GESTURE
	};
};

struct wlr_action_binding_v1 {
	struct wl_resource *resource;
	struct wlr_action_binder_v1_state *state;

	char *category, *name;
	char *description; // may be NULL when the client doesn't set a description

	struct wlr_action_binding_hint_v1 hint;
	char *app_id; // may be NULL when the client doesn't set an app_id
	struct wlr_seat *seat; // may be NULL when the client doesn't set a seat
	struct wl_listener seat_destroy;

	struct {
		struct wl_signal destroy;
	} events;

	bool bound;
	struct wl_list link;
};

struct wlr_action_binder_v1 *wlr_action_binder_v1_create(struct wl_display *display);
void wlr_action_binding_v1_bind(struct wlr_action_binding_v1 *bind, const char *trigger);
void wlr_action_binding_v1_reject(struct wlr_action_binding_v1 *bind);
void wlr_action_binding_v1_trigger(struct wlr_action_binding_v1 *binding, uint32_t trigger_type, uint32_t time_msec);

#endif
