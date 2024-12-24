#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wayland-util.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output_group.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>
#include "backend/drm/drm.h"
#include "render/drm_format_set.h"

static const struct wlr_output_impl output_impl;
static const struct wlr_backend_impl backend_impl;
static void output_group_child_destroy(struct wlr_output_group_child *child);
static void output_group_state_change(void *data);

// A global registry for output groups.
static struct wl_list priv_registry;
static struct wl_list *registry = NULL;

static bool backend_is_group(struct wlr_backend *b) {
	return (b->impl == &backend_impl);
}

static struct wlr_output_group *group_from_output(struct wlr_output *output) {
	assert(output->impl == &output_impl);
	return (struct wlr_output_group *)output;
}

static struct wlr_output_group *group_from_backend(struct wlr_backend *wlr_backend) {
	assert(backend_is_group(wlr_backend));
	struct wlr_output_group *group = wl_container_of(wlr_backend, group, backend);
	return group;
}

static int backend_get_drm_fd(struct wlr_backend *backend) {
	struct wlr_output_group *group = group_from_backend(backend);
	struct wlr_output_group_child *primary =
		wl_container_of(group->children.next, primary, link);
	struct wlr_backend *child_backend = primary->output->backend;
	if (child_backend->impl->get_drm_fd)
		return child_backend->impl->get_drm_fd(child_backend);
	return -1;
}

static uint32_t backend_get_buffer_caps(struct wlr_backend *backend) {
	struct wlr_output_group *group = group_from_backend(backend);
	struct wlr_output_group_child *primary =
		wl_container_of(group->children.next, primary, link);
	struct wlr_backend *child_backend = primary->output->backend;
	if (child_backend->impl->get_buffer_caps)
		return child_backend->impl->get_buffer_caps(child_backend);
	return 0;
}

static void handle_present(struct wl_listener *listener, void *user_data) {
	struct wlr_output_event_present *event = (struct wlr_output_event_present *) user_data;
	struct wlr_output_group_child *child = wl_container_of(listener, child, present);
	struct wlr_output_group *group = child->group;
	if (group->queued_frame_count > 0) {
		group->queued_frame_count -= 1;
	}
	if (group->queued_frame_count == 0) {
		wlr_output_send_present(&group->output, event);
	}
}

static void handle_frame(struct wl_listener *listener, void *user_data) {
	struct wlr_output_group_child *child = wl_container_of(listener, child, frame);
	struct wlr_output_group *group = child->group;
	/* present happens before frame so trust that backend already did present */
	if (group->queued_frame_count == 0) {
		wl_signal_emit_mutable(&group->output.events.frame, &group->output);
	}
}

static void handle_needs_frame(struct wl_listener *listener, void *user_data) {
	struct wlr_output *output = (struct wlr_output *) user_data;
	struct wlr_output_group_child *child = wl_container_of(listener, child, needs_frame);
	struct wlr_output_group *group = child->group;
	/* if any output raises needs_frame, re-raise it */
	output->needs_frame = false;
	wlr_output_update_needs_frame(&group->output);
}

static void handle_output_destroy(struct wl_listener *listener, void *user_data) {
	struct wlr_output_group_child *child = wl_container_of(listener, child, output_destroy);
	output_group_child_destroy(child);
}

struct wlr_output_group_mode {
	struct wlr_output_mode mode;
	struct wlr_output_mode *original_mode;
};

#define GROUP_PREFIX "GROUP-"
static bool clone_output(struct wlr_output_group *group, struct wlr_output *src_output,
	struct wlr_output_group_tile_info *tile_info) {
	struct wlr_output *dst_output = &group->output;

	size_t new_name_len = sizeof(GROUP_PREFIX) + strlen(src_output->name);
	char *new_name = malloc(new_name_len);
	if (!new_name)
		return false;
	snprintf(new_name, new_name_len, "%s%s", GROUP_PREFIX, src_output->name);

	wlr_output_init(dst_output, &group->backend, &output_impl, src_output->event_loop, NULL);
	wlr_output_set_description(dst_output, src_output->description);
	dst_output->name = new_name;
	dst_output->make = strdup(src_output->make);
	dst_output->model = strdup(src_output->model);
	dst_output->serial = strdup(src_output->serial);
	dst_output->phys_width = src_output->phys_width;
	dst_output->phys_height = src_output->phys_height;
	dst_output->current_mode = src_output->current_mode;
	dst_output->width = src_output->width;
	dst_output->height = src_output->height;
	dst_output->refresh = src_output->refresh;
	dst_output->enabled = src_output->enabled;
	dst_output->scale = src_output->scale;
	dst_output->subpixel = src_output->subpixel;
	dst_output->transform = src_output->transform;
	dst_output->adaptive_sync_status = src_output->adaptive_sync_status;

	/* Clone all child modes while but keep references to the original. This is
	 * needed because the drm backend uses its own mode container
	 * (wlr_drm_mode) to link a wlr_mode to a drmModeModeInfo. */
	struct wlr_output_mode *mode;
	wl_list_for_each_reverse(mode, &src_output->modes, link) {
		struct wlr_output_group_mode *group_mode = calloc(1, sizeof(*group_mode));
		memcpy(&group_mode->mode, mode, sizeof(*mode));
		group_mode->original_mode = mode;
		wl_list_insert(&dst_output->modes, &group_mode->mode.link);
	}
	return true;
}

struct wlr_output_group *wlr_output_group_match_tile(struct wlr_output_group_tile_info *tile_info) {
	if (!registry) {
		return NULL;
	}
	struct wlr_output_group *group;
	wl_list_for_each(group, registry, link) {
		struct wlr_output_group_child *child = wl_container_of(group->children.next, child, link);
		if (child->tile_info.group_id == tile_info->group_id) {
			return group;
		}
	}
	return NULL;
}

void wlr_output_group_add_tile(struct wlr_output_group *group, struct wlr_output *output,
	struct wlr_output_group_tile_info *tile_info) {
	struct wlr_output_group_child *child = calloc(1, sizeof(*child));
	assert(tile_info->group_id != 0);
	child->output = output;

	child->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->events.destroy, &child->output_destroy);
	child->frame.notify = handle_frame;
	wl_signal_add(&output->events.frame, &child->frame);

	child->present.notify = handle_present;
	wl_signal_add(&output->events.present, &child->present);
	child->needs_frame.notify = handle_needs_frame;
	wl_signal_add(&output->events.needs_frame, &child->needs_frame);

	child->group = group;
	child->tile_info = *tile_info;

	/* index is like array v,h:
	 * #1 [0,0], #2 [0,1], #3 [0,2],
	 * #4 [1,0], #5 [1,1], #6 [1,2],
	 * #7 [2,0], #8 [2,1], #9 [2,2],
	 */
	child->index = (tile_info->v_loc * tile_info->num_h) + tile_info->h_loc;

	/* sorted insert to keep children in order for calculating tiled mode */
	struct wlr_output_group_child *cur;
	wl_list_for_each(cur, &group->children, link) {
		if (child->index < cur->index) {
			break;
		}
	}
	wl_list_insert(cur->link.prev, &child->link);

	if (child->output->backend->features.timeline == false) {
		group->backend.features.timeline = false;
	}

	if (group->ready == NULL) {
		group->ready = wl_event_loop_add_idle(child->output->event_loop,
		    output_group_state_change, group);
	}
}

struct wlr_output_group *wlr_output_group_create(void) {
	if (registry == NULL) {
		wl_list_init(&priv_registry);
		registry = &priv_registry;
	}
	struct wlr_output_group *group = calloc(1, sizeof(*group));
	wl_list_insert(registry, &group->link);
	wl_list_init(&group->children);
	wlr_backend_init(&group->backend, &backend_impl);
	group->backend.features.timeline = true;
	return group;
}

static void output_group_destroy(struct wlr_output *output) {
	struct wlr_output_group *group = group_from_output(output);
	struct wlr_output_group_child *child,*child_tmp;
	wl_list_for_each_safe(child, child_tmp, &group->children, link) {
		output_group_child_destroy(child);
	}
	struct wlr_output_group_mode *mode, *mode_tmp;
	wl_list_for_each_safe(mode, mode_tmp, &group->output.modes, mode.link) {
		wl_list_remove(&mode->mode.link);
		free(mode);
	}
	if (group->ready != NULL) {
		wl_event_source_remove(group->ready);
	}
	free(group->cursor_sizes);
	wl_list_remove(&group->link);
	free(group);
}

static void output_group_child_destroy(struct wlr_output_group_child *child) {
	struct wlr_output_group *group = child->group;
	wlr_log(WLR_DEBUG, "removing child %s from group %s",
		child->output->name, group->output.name);
	wl_list_remove(&child->present.link);
	wl_list_remove(&child->needs_frame.link);
	wl_list_remove(&child->frame.link);
	wl_list_remove(&child->output_destroy.link);
	wl_list_remove(&child->link);
	/* Schedule a group state change event. When all children are removed, the
	 * output will be destroyed. */
	if (group->ready == NULL) {
		group->ready = wl_event_loop_add_idle(child->output->event_loop,
		    output_group_state_change, group);
	}
	free(child);
}

static bool output_group_commit_helper(struct wlr_output *parent, const struct wlr_output_state *state, bool test) {
	struct wlr_output_group *group = group_from_output(parent);
	bool ret = false;
	bool failed = false;
	bool in_tiled_mode = false;

	if(state->committed & WLR_OUTPUT_STATE_MODE) {
		if (state->mode == group->tiled_mode) {
			in_tiled_mode = true;
		}
	} else {
		if (parent->current_mode == group->tiled_mode) {
			in_tiled_mode = true;
		}
	}

	/* TODO: it is probably possible to figure out crop/scaling for children */
	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		if (state->buffer_dst_box.x != 0 || state->buffer_dst_box.y != 0 ||
		    state->buffer_src_box.x != 0.0 || state->buffer_src_box.y != 0.0) {
			wlr_log(WLR_DEBUG, "crop/scaling not implemented in output group");
			return false;
		}
	}

	struct wlr_output_group_child *child;
	bool single_output_enabled = false;
	int frame_count = 0;
	wl_list_for_each(child, &group->children, link) {
		struct wlr_output *output = child->output;
		struct wlr_output_state state_copy = *state;
		struct wlr_output_state *pending = &state_copy;

		/* commit_seq important for presentation feedback! */
		output->commit_seq = parent->commit_seq;

		if (in_tiled_mode) {
			frame_count += 1;
			pending->buffer_src_box = child->src_box;
			pending->buffer_dst_box = child->dst_box;
			pending->mode = child->tiled_mode;
			if (output->enabled == false && !(pending->committed & WLR_OUTPUT_STATE_ENABLED)) {
				pending->committed |= WLR_OUTPUT_STATE_ENABLED;
				pending->enabled = true;
			}
		} else {
			frame_count = 1;
			if (output->enabled == true || (pending->committed & WLR_OUTPUT_STATE_ENABLED && pending->enabled == true)) {
				if (single_output_enabled == false) {
					/* first child gets turned on */
					if ((pending->committed & WLR_OUTPUT_STATE_MODE) && (pending->mode_type == WLR_OUTPUT_STATE_MODE_FIXED)) {
						struct wlr_output_group_mode *group_mode = wl_container_of(pending->mode, group_mode, mode);
						pending->mode = group_mode->original_mode;
					}
					single_output_enabled = true;
				} else {
					/* rest of the children get forced off */
					pending->committed = WLR_OUTPUT_STATE_ENABLED;
					pending->enabled = false;
				}
			}
		}

		if (output->enabled == false && !(pending->committed & WLR_OUTPUT_STATE_ENABLED)) {
			continue;
		}

		/* The parent group output is going through wlr_output_commit_state()
		 * and managing state. Skip directly to the children output
		 * implementation commit. */

		if (test) {
			if (output->impl->test) {
				if (false == output->impl->test(output, pending)) {
					return false;
				}
			}
		} else {
			ret = output->impl->commit(output, pending);
			if(ret == false) {
				failed = true;
				wlr_log(WLR_DEBUG, "commit failed on %s", output->name);
			}
			if (!failed) {
				output_apply_state(output, pending);
				if (output->frame_pending) {
					parent->frame_pending = true;
				}
			}
		}
	}

	if (test) {
		return true;
	} else if (failed) {
		/* Do not present any frame where any children failed */
		group->queued_frame_count = -1;
	} else {
		/* Synchronize all children outputs to prevent tearing. Make
		 * sure we get all the children frame/present events before
		 * forwarding that to the group output. */
		group->queued_frame_count = frame_count;
	}

	return !failed;
}

static bool output_group_commit(struct wlr_output *parent, const struct wlr_output_state *state) {
	return output_group_commit_helper(parent, state, false);
}

static bool output_group_test(struct wlr_output *parent, const struct wlr_output_state *state) {
	return output_group_commit_helper(parent, state, true);
}

static size_t output_group_get_gamma_size(struct wlr_output *output) {
	struct wlr_output_group *group = group_from_output(output);
	size_t gamma_size = 0;
	size_t tmp_gamma_size = 0;
	struct wlr_output_group_child *child;
	wl_list_for_each(child, &group->children, link) {
		if (child->output->impl->get_gamma_size) {
			tmp_gamma_size = child->output->impl->get_gamma_size(child->output);
		}
		if (gamma_size == 0) {
			gamma_size = tmp_gamma_size;
		}
		if (tmp_gamma_size == 0 || tmp_gamma_size != gamma_size) {
			return 0;
		}
	}
	return gamma_size;
}

static bool output_group_set_cursor(struct wlr_output *output,
		struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
	struct wlr_output_group *group = group_from_output(output);
	struct wlr_output_group_child *child;
	wl_list_for_each(child, &group->children, link) {
		if (child->output->enabled) {
			child->output->impl->set_cursor(child->output, buffer, hotspot_x, hotspot_y);
		}
	};
	return true;
}

static bool output_group_move_cursor(struct wlr_output *output,
		int x, int y) {
	struct wlr_output_group *group = group_from_output(output);
	struct wlr_output *parent = &group->output;
	struct wlr_output_group_child *child;
	/* copied from backend/drm.c ;-) */
	struct wlr_box box = { .x = x, .y = y };
	int width, height;
	enum wl_output_transform transform = wlr_output_transform_invert(parent->transform);
	wlr_output_transformed_resolution(output, &width, &height);
	wlr_box_transform(&box, &box, transform, width, height);
	wl_list_for_each(child, &group->children, link) {
		if (child->output->enabled) {
			child->output->impl->move_cursor(child->output, box.x - child->src_box.x, box.y - child->src_box.y);
		}
	};
	return true;
}

static const struct wlr_output_cursor_size *output_group_get_cursor_sizes(struct wlr_output *output, size_t *len) {
	struct wlr_output_group *group = group_from_output(output);
	*len = group->cursor_sizes_len;
	return group->cursor_sizes;
}

static const struct wlr_drm_format_set *output_group_get_cursor_formats(
		struct wlr_output *output, uint32_t buffer_caps) {
	struct wlr_output_group *group = group_from_output(output);
	struct wlr_output_group_child *child;
	bool first = true;
	wl_list_for_each(child, &group->children, link) {
		if (!child->output->impl->get_cursor_formats) {
			wlr_drm_format_set_finish(&group->cursor_formats);
			break;
		}
		const struct wlr_drm_format_set *set =
			child->output->impl->get_cursor_formats(child->output, buffer_caps);
		if (first) {
			wlr_drm_format_set_copy(&group->cursor_formats, set);
			first = false;
		} else {
			wlr_drm_format_set_intersect(&group->cursor_formats, &group->cursor_formats, set);
		}
	}
	return &group->cursor_formats;
}

static const struct wlr_drm_format_set *output_group_get_primary_formats(
		struct wlr_output *output, uint32_t buffer_caps) {
	struct wlr_output_group *group = group_from_output(output);
	bool first = true;
	struct wlr_output_group_child *child;
	wl_list_for_each(child, &group->children, link) {
		if (!child->output->impl->get_primary_formats) {
			wlr_drm_format_set_finish(&group->primary_formats);
			break;
		}
		const struct wlr_drm_format_set *set =
			child->output->impl->get_primary_formats(child->output, buffer_caps);
		if (first) {
			wlr_drm_format_set_copy(&group->primary_formats, set);
			first = false;
		} else {
			wlr_drm_format_set_intersect(&group->primary_formats, &group->primary_formats, set);
		}
	}
	return &group->primary_formats;
}

static void calculate_and_allocate_tiled_mode(struct wlr_output_group *group) {
	struct wlr_output_group_mode *group_mode = calloc(1, sizeof(*group_mode));
	uint32_t x_start = 0, y_start = 0;
	struct wlr_output_group_child *child;

	wl_list_for_each(child, &group->children, link) {
		struct wlr_output_group_tile_info *tile_info = &child->tile_info;

		/* this depends on iterating through the children in tile index order and
		 * assumes the dimensions work */
		if (tile_info->v_loc == 0) {
			group_mode->mode.width += tile_info->h_size;
		}
		if (tile_info->h_loc == 0) {
			group_mode->mode.height += tile_info->v_size;
		}

		/* Generate the crop for this specific tile. The source buffer is shared
		 * between all tiles and each child output takes a subset of the shared
		 * buffer. */
		child->src_box.x = x_start;
		child->src_box.y = y_start;
		child->src_box.width = tile_info->h_size;
		child->src_box.height = tile_info->v_size;

		child->dst_box.x = 0;
		child->dst_box.y = 0;
		child->dst_box.width = tile_info->h_size;
		child->dst_box.height = tile_info->v_size;

		if (tile_info->h_loc == (tile_info->num_h-1)) {
			x_start = 0;
			y_start += tile_info->v_size;
		} else {
			x_start += tile_info->h_size;
		}

		struct wlr_output_mode *mode;
		wl_list_for_each(mode, &child->output->modes, link) {
			if (mode->width == (int32_t)tile_info->h_size && mode->height == (int32_t)tile_info->v_size) {
				child->tiled_mode = mode;
				if ((group_mode->mode.refresh == 0) || (mode->refresh < group_mode->mode.refresh)) {
					/* slowest refresh wins */
					group_mode->mode.refresh = mode->refresh;
				}
				break;
			}
		}
	}
	//TODO: set aspect ratio?
	group_mode->mode.picture_aspect_ratio = WLR_OUTPUT_MODE_ASPECT_RATIO_NONE;
	group_mode->mode.preferred = true;
	group->tiled_mode = &group_mode->mode;
	wl_list_insert(&group->output.modes, &group_mode->mode.link);
}

static bool is_cursor_size_in_all_children(struct wlr_output_group *group, struct wlr_output_group_child *myself, const struct wlr_output_cursor_size *size) {
	struct wlr_output_group_child *child;
	wl_list_for_each(child, &group->children, link) {
		size_t len = 0;
		if (child == myself) {
			continue;
		}
		if (!child->output->impl->get_cursor_sizes) {
			continue;
		}
		const struct wlr_output_cursor_size *sizes = child->output->impl->get_cursor_sizes(child->output, &len);
		bool found = false;
		for (size_t i=0; i < len && found == false; ++i) {
			if (sizes[i].width == size->width && sizes[i].height == size->height) {
				found = true;
			}
		};
		if (!found) {
			return false;
		}
	}
	return true;
}

static void allocate_cursor_sizes(struct wlr_output_group *group) {
	struct wlr_output_group_child *child;
	size_t cur_idx = 0;

    /* Save the intersection of hardware cursor sizes. */
	wl_list_for_each(child, &group->children, link) {
		size_t len = 0;
		if (!child->output->impl->get_cursor_sizes) {
			continue;
		}
		const struct wlr_output_cursor_size *sizes = child->output->impl->get_cursor_sizes(child->output, &len);
		group->cursor_sizes = calloc(len, sizeof(struct wlr_output_cursor_size));
		for (size_t i=0; i<len; ++i) {
			if (is_cursor_size_in_all_children(group, child, &sizes[i])) {
				group->cursor_sizes[cur_idx] = sizes[i];
				cur_idx += 1;
			}
		}
		group->cursor_sizes_len = cur_idx;

		break;
	}
}

static void output_group_state_change(void *data) {
	struct wlr_output_group *group = data;
	int num_children = wl_list_length(&group->children);
	bool need_init = false;
	bool need_destroy = false;
	if (group->num_children > 0) {
		need_destroy = true;
	}

	if (num_children > 0) {
		need_init = true;
	}

	if (need_destroy) {
		struct wlr_output_group *old_group = group;
		if (need_init) {
			struct wlr_output_group *new_group = wlr_output_group_create();
			struct wlr_output_group_child *child, *child_tmp;

			/* prevent re-entering */
			new_group->ready = old_group->ready;

			/* disable old group */
			const struct wlr_output_state pending = (struct wlr_output_state) {
				.committed = WLR_OUTPUT_STATE_ENABLED,
				.allow_reconfiguration = true,
				.enabled = false,
			};
			wlr_output_commit_state(&old_group->output, &pending);

			/* move children to new group */
			wl_list_for_each_safe(child, child_tmp, &group->children, link) {
				wlr_output_group_add_tile(new_group, child->output, &child->tile_info);
				output_group_child_destroy(child);
			}

			/* old group will get free'd during output destroy */
			group = new_group;
		}

		wlr_output_destroy(&old_group->output);
	}

	group->ready = NULL;
	group->num_children = num_children;

	if (!need_init) {
		return;
	}

	/* the first child is the primary */
	struct wlr_output_group_child *primary =
		wl_container_of(group->children.next, primary, link);

	if (!clone_output(group, primary->output, &primary->tile_info))
		return;

	/* try to support a partial tiled array */
	int num_children_needed;
	if (!primary->tile_info.is_single_monitor) {
		num_children_needed = (primary->tile_info.num_h * primary->tile_info.num_v);
	} else {
		num_children_needed = 1;
	}

	if (num_children >= num_children_needed) {
		calculate_and_allocate_tiled_mode(group);
	}
	allocate_cursor_sizes(group);

	wlr_log(WLR_INFO, "created output group %s (%dx%d mm)",
		group->output.name, group->output.phys_width, group->output.phys_height);

	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &group->output.modes, link) {
		wlr_log(WLR_DEBUG, "  mode %dx%d@%d %s %s",
			mode->width, mode->height, mode->refresh,
			mode->preferred?"(preferred)":"",
			(mode == group->tiled_mode)?"(tiled)":"");
	}
	wl_signal_emit_mutable(&primary->output->backend->events.new_output, &group->output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_group_destroy,
	.test = output_group_test,
	.commit = output_group_commit,
	.get_gamma_size = output_group_get_gamma_size,
	.set_cursor = output_group_set_cursor,
	.move_cursor = output_group_move_cursor,
	.get_cursor_formats = output_group_get_cursor_formats,
	.get_cursor_sizes = output_group_get_cursor_sizes,
	.get_primary_formats = output_group_get_primary_formats,
};

static const struct wlr_backend_impl backend_impl = {
	.start = NULL,
	.destroy = NULL,
	.get_drm_fd = backend_get_drm_fd,
	.get_buffer_caps = backend_get_buffer_caps,
	.test = NULL,
	.commit = NULL,
};
