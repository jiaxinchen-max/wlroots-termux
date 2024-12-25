#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

/* Inspect outputs and perform basic operations for testing.
 *
 * Escape:     Exit
 * Left/Right: Select output
 * Up/Down:    Select mode
 * Return:     Activate selected mode
 * Backspace:  Disable selected output
 * Space:      Enable selected output
 * Delete:     Disable ALL outputs
 * Insert:     Enable ALL outputs
 *
 * Don't swop outputs around while this is running, or weird stuff will happen.
 * This is by design. Or, rather, lack of design. */

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wl_list outputs;
	struct wl_listener new_output;
	struct wl_listener new_input;
	struct output *selected_output;

	struct wl_list keyboards;
};

struct output {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct server *server;
	char *name;
	struct wlr_texture *texture;
	int selected_mode_idx;
	bool enabled;

	struct wl_listener frame;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener key;
};

/* Output handling */

static void handle_output_frame(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_output_state output_state;
	wlr_output_state_init(&output_state);
	struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &output_state, NULL, NULL);
	if (output->texture) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = output->texture,
			.transform = wlr_output->transform
		});
	}
	else {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = { .width = wlr_output->width, .height = wlr_output->height },
			.color = { 0, 0, 0, 1 }
		});
	}
	wlr_render_pass_submit(pass);
	wlr_output_commit_state(wlr_output, &output_state);
	wlr_output_state_finish(&output_state);
}

static void update_output_texture(struct output *output) {
	if (!output->enabled) {
		return;
	}

	if (output->texture) {
		wlr_texture_destroy(output->texture);
	}

	int width = output->wlr_output->width;
	int height = output->wlr_output->height;

	struct wlr_output *wlr_output = output->wlr_output;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	double base_scale = height * 0.02;
	double margin = base_scale * 3;
	double large = base_scale * 3;
	double medium = base_scale * 1.5;
	double small = base_scale * 0.75;

	double y = margin;

	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_select_font_face(cr, "cairo:monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

	if (output == output->server->selected_output) {
		cairo_set_line_width(cr, small * 2);
		cairo_rectangle(cr, 0, 0, width, height);
		cairo_stroke(cr);
	}

	cairo_set_font_size(cr, large);
	cairo_move_to(cr, margin, (y += large));
	cairo_show_text(cr, wlr_output->name);
	cairo_set_font_size(cr, medium);
	cairo_move_to(cr, margin, (y += medium));
	cairo_show_text(cr, wlr_output->description);
	y += medium;

	struct wlr_output_mode *mode;
	char mode_txt[1024];
	cairo_set_font_size(cr, small);
	int idx = 0;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		cairo_move_to(cr, margin, (y += small));
		if (output->selected_mode_idx == idx++) {
			cairo_set_source_rgb(cr, 1, 1, 1);
		}
		else {
			cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
		}
		snprintf(mode_txt, sizeof(mode_txt), "%d x %d @ %d %s %s", mode->width, mode->height,
				mode->refresh, mode->preferred ? "(Preferred)" : "",
				wlr_output->current_mode == mode ? "(Active)" : "");
		cairo_show_text(cr, mode_txt);
	}
	cairo_destroy(cr);

	output->texture = wlr_texture_from_pixels(output->server->renderer, DRM_FORMAT_ARGB8888,
		cairo_image_surface_get_stride(surface), width, height,
		cairo_image_surface_get_data(surface));

	cairo_surface_destroy(surface);

	wlr_output_schedule_frame(output->wlr_output);
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = data;
	if ((event->state->committed & WLR_OUTPUT_STATE_MODE) != 0) {
		update_output_texture(output);
	}
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, destroy);
	wlr_texture_destroy(output->texture);
	output->texture = NULL;
	output->wlr_output = NULL;
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct output *output = NULL;
	struct output *tmp;
	wl_list_for_each(tmp, &server->outputs, link) {
		if (strcmp(wlr_output->name, tmp->name) == 0) {
			output = tmp;
			break;
		}
	}

	if (output == NULL) {
		output = calloc(1, sizeof(*output));
		output->server = server;
		output->name = strdup(wlr_output->name);
		output->selected_mode_idx = -1;
		output->enabled = true;

		wl_list_insert(&server->outputs, &output->link);
	}

	output->wlr_output = wlr_output;

	output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->commit.notify = handle_output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	if (server->selected_output == NULL) {
		server->selected_output = output;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (output->enabled) {
		wlr_output_state_set_enabled(&state, true);
	}
	struct wlr_output_mode *mode = NULL;
	int idx = 0;
	if (wl_list_empty(&wlr_output->modes)) {
		mode = wlr_output_preferred_mode(wlr_output);
	}
	else {
		wl_list_for_each(mode, &wlr_output->modes, link) {
			if ((output->selected_mode_idx < 0 && mode->preferred)
					|| idx == output->selected_mode_idx) {
				break;
			}
			idx++;
		}
	}
	output->selected_mode_idx = idx;
	wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	if (output->enabled) {
		update_output_texture(output);
	}
}

static void output_enable(struct output *output, bool enabled) {
	if (!output->wlr_output) {
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enabled);
	wlr_output_commit_state(output->wlr_output, &state);
	wlr_output_state_finish(&state);
	output->enabled = enabled;
}

/* Input handling */

static void handle_key_command(struct server *server, const char *sym) {
	if (strcmp("Escape", sym) == 0) {
		wl_display_terminate(server->display);
	}

	struct output *output = server->selected_output;
	if (!output) {
		return;
	}
	if (strcmp("Down", sym) == 0) {
		output->selected_mode_idx++;
		if (output->selected_mode_idx >= wl_list_length(&output->wlr_output->modes)) {
			output->selected_mode_idx = 0;
		}
		update_output_texture(output);
	}
	else if (strcmp("Up", sym) == 0) {
		output->selected_mode_idx--;
		if (output->selected_mode_idx < 0) {
			output->selected_mode_idx = wl_list_length(&output->wlr_output->modes) - 1;
		}
		update_output_texture(output);
	}
	else if (strcmp("Return", sym) == 0) {
		struct wlr_output_mode *mode;
		int idx = 0;
		wl_list_for_each(mode, &output->wlr_output->modes, link) {
			if (output->selected_mode_idx == idx++) {
				struct wlr_output_state state;
				wlr_output_state_init(&state);
				wlr_output_state_set_mode(&state, mode);
				wlr_output_commit_state(output->wlr_output, &state);
				wlr_output_state_finish(&state);
				break;
			}
		}
	}
	else if (strcmp("Right", sym) == 0) {
		struct wl_list *next = output->link.prev == &server->outputs
			? server->outputs.prev : output->link.prev;
		if (next != &output->link) {
			server->selected_output = wl_container_of(next, server->selected_output, link);
			update_output_texture(output);
			update_output_texture(server->selected_output);
		}
	}
	else if (strcmp("Left", sym) == 0) {
		struct wl_list *prev = output->link.next == &server->outputs
			? server->outputs.next : output->link.next;
		if (prev != &output->link) {
			server->selected_output = wl_container_of(prev, server->selected_output, link);
			update_output_texture(output);
			update_output_texture(server->selected_output);
		}
	}
	else if (strcmp("BackSpace", sym) == 0) {
		output_enable(output, false);
	}
	/* Yeah, it's supposed to be lowercase. */
	else if (strcmp("space", sym) == 0) {
		output_enable(output, true);
	}
	else if (strcmp("Delete", sym) == 0) {
		wl_list_for_each(output, &server->outputs, link) {
			output_enable(output, false);
		}
	}
	else if (strcmp("Insert", sym) == 0) {
		wl_list_for_each(output, &server->outputs, link) {
			output_enable(output, true);
		}
	}
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	if (event->state != 1) {
		return;
	}
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, event->keycode + 8, &syms);
	char sym_name[64];
	for (int i = 0; i < nsyms; i++) {
		if (xkb_keysym_get_name(syms[i], &sym_name[0], sizeof(sym_name)) >= 0) {
			handle_key_command(server, sym_name);
		}
	}
}

static void handle_new_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	if (device->type != WLR_INPUT_DEVICE_KEYBOARD) {
		return;
	}

	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->key.notify = handle_keyboard_key;
	wl_list_insert(&server->keyboards, &keyboard->link);
}

int main(void) {
	wlr_log_init(WLR_DEBUG, NULL);

	struct server server = {0};
	server.display = wl_display_create();
	server.backend = wlr_backend_autocreate(server.display, NULL);

	server.renderer = wlr_renderer_autocreate(server.backend);
	wlr_renderer_init_wl_display(server.renderer, server.display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

	wl_list_init(&server.outputs);
	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	wl_list_init(&server.keyboards);
	server.new_input.notify = handle_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);

	if (!wlr_backend_start(server.backend)) {
		wl_display_destroy(server.display);
		return EXIT_FAILURE;
	}

	wl_display_run(server.display);

	wl_display_destroy(server.display);
	return EXIT_SUCCESS;
}
