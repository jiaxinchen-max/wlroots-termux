/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FOREIGN_TOPLEVEL_MANAGEMENT_V1_H
#define WLR_TYPES_WLR_FOREIGN_TOPLEVEL_MANAGEMENT_V1_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

struct wlr_foreign_toplevel_manager_v1 {
	struct wl_event_loop *event_loop;
	struct wl_global *global;
	struct wl_list resources; // wl_resource_get_link()
	struct wl_list toplevels; // wlr_foreign_toplevel_handle_v1.link

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

enum wlr_foreign_toplevel_handle_v1_state {
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED         = (1 << 0),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED         = (1 << 1),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED         = (1 << 2),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN        = (1 << 3),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ALWAYS_ON_TOP     = (1 << 4),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ON_ALL_WORKSPACES = (1 << 5),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ROLLED_UP         = (1 << 6),
	WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_URGENT            = (1 << 7),
};

struct wlr_foreign_toplevel_handle_v1_output {
	struct wl_list link; // wlr_foreign_toplevel_handle_v1.outputs
	struct wlr_output *output;
	struct wlr_foreign_toplevel_handle_v1 *toplevel;

	struct {
		struct wl_listener output_bind;
		struct wl_listener output_destroy;
	} WLR_PRIVATE;
};

struct wlr_foreign_toplevel_handle_v1 {
	struct wlr_foreign_toplevel_manager_v1 *manager;
	struct wl_list resources;
	struct wl_list link;
	struct wl_event_source *idle_source;

	char *title;
	char *app_id;
	struct wlr_foreign_toplevel_handle_v1 *parent;
	struct wl_list outputs; // wlr_foreign_toplevel_v1_output.link
	uint32_t state; // enum wlr_foreign_toplevel_v1_state

	struct {
		// struct wlr_foreign_toplevel_handle_v1_maximized_event
		struct wl_signal request_maximize;
		// struct wlr_foreign_toplevel_handle_v1_minimized_event
		struct wl_signal request_minimize;
		// struct wlr_foreign_toplevel_handle_v1_activated_event
		struct wl_signal request_activate;
		// struct wlr_foreign_toplevel_handle_v1_fullscreen_event
		struct wl_signal request_fullscreen;
		struct wl_signal request_close;
		struct wl_signal request_always_on_top;
		struct wl_signal request_on_all_workspaces;
		struct wl_signal request_roll_up;

		// struct wlr_foreign_toplevel_handle_v1_set_rectangle_event
		struct wl_signal set_rectangle;
		struct wl_signal destroy;
	} events;

	void *data;
};

struct wlr_foreign_toplevel_handle_v1_maximized_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	bool maximized;
};

struct wlr_foreign_toplevel_handle_v1_minimized_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	bool minimized;
};

struct wlr_foreign_toplevel_handle_v1_activated_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	struct wlr_seat *seat;
};

struct wlr_foreign_toplevel_handle_v1_fullscreen_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	bool fullscreen;
	struct wlr_output *output;
};

struct wlr_foreign_toplevel_handle_v1_always_on_top_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	bool always_on_top;
};

struct wlr_foreign_toplevel_handle_v1_on_all_workspaces_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	bool on_all_workspaces;
};

struct wlr_foreign_toplevel_handle_v1_roll_up_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	bool roll_up;
};

struct wlr_foreign_toplevel_handle_v1_set_rectangle_event {
	struct wlr_foreign_toplevel_handle_v1 *toplevel;
	struct wlr_surface *surface;
	int32_t x, y, width, height;
};

struct wlr_foreign_toplevel_manager_v1 *wlr_foreign_toplevel_manager_v1_create(
	struct wl_display *display);

struct wlr_foreign_toplevel_handle_v1 *wlr_foreign_toplevel_handle_v1_create(
	struct wlr_foreign_toplevel_manager_v1 *manager);
/**
 * Destroy the given toplevel handle, sending the closed event to any
 * client. Also, if the destroyed toplevel is set as a parent of any
 * other valid toplevel, clients still holding a handle to both are
 * sent a parent signal with NULL parent. If this is not desired, the
 * caller should ensure that any child toplevels are destroyed before
 * the parent.
 */
void wlr_foreign_toplevel_handle_v1_destroy(
	struct wlr_foreign_toplevel_handle_v1 *toplevel);

void wlr_foreign_toplevel_handle_v1_set_title(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, const char *title);
void wlr_foreign_toplevel_handle_v1_set_app_id(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, const char *app_id);

void wlr_foreign_toplevel_handle_v1_output_enter(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, struct wlr_output *output);
void wlr_foreign_toplevel_handle_v1_output_leave(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, struct wlr_output *output);

void wlr_foreign_toplevel_handle_v1_set_maximized(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, bool maximized);
void wlr_foreign_toplevel_handle_v1_set_minimized(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, bool minimized);
void wlr_foreign_toplevel_handle_v1_set_activated(
	struct wlr_foreign_toplevel_handle_v1 *toplevel, bool activated);
void wlr_foreign_toplevel_handle_v1_set_fullscreen(
	struct wlr_foreign_toplevel_handle_v1* toplevel, bool fullscreen);
void wlr_foreign_toplevel_handle_v1_set_always_on_top(
	struct wlr_foreign_toplevel_handle_v1* toplevel, bool always_on_top);
void wlr_foreign_toplevel_handle_v1_set_on_all_workspaces(
	struct wlr_foreign_toplevel_handle_v1* toplevel, bool on_all_workspaces);
void wlr_foreign_toplevel_handle_v1_set_rolled_up(
	struct wlr_foreign_toplevel_handle_v1* toplevel, bool rolled_up);
void wlr_foreign_toplevel_handle_v1_set_urgent(
	struct wlr_foreign_toplevel_handle_v1* toplevel, bool urgent);

/**
 * Set the parent of a toplevel. If the parent changed from its previous
 * value, also sends a parent event to all clients that hold handles to
 * both toplevel and parent (no message is sent to clients that have
 * previously destroyed their parent handle). NULL is allowed as the
 * parent, meaning no parent exists.
 */
void wlr_foreign_toplevel_handle_v1_set_parent(
	struct wlr_foreign_toplevel_handle_v1 *toplevel,
	struct wlr_foreign_toplevel_handle_v1 *parent);


#endif
