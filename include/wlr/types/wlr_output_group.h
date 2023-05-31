#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_OUTPUT_GROUP_H
#define WLR_TYPES_WLR_OUTPUT_GROUP_H

#include <stdint.h>
#include <wlr/types/wlr_output_group.h>
#include <wlr/types/wlr_output.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/box.h>
#include <wlr/backend.h>

struct wlr_output_group_tile_info {
	uint32_t group_id;
	uint32_t is_single_monitor;
	uint32_t num_h;
	uint32_t num_v;
	uint32_t h_loc;
	uint32_t v_loc;
	uint32_t h_size;
	uint32_t v_size;
};

struct wlr_output_group_child {
	struct wlr_output *output;
	struct wlr_output_group *group;
	struct wlr_fbox src_box;
	struct wlr_box dst_box;
	struct wlr_output_group_tile_info tile_info;
	uint32_t index;
	struct wlr_output_mode *tiled_mode;
	struct wl_listener present;
	struct wl_listener frame;
	struct wl_listener needs_frame;
	struct wl_listener output_destroy;
	struct wl_list link;
};

struct wlr_output_group {
	struct wlr_output output;
	/* private data below */
	int queued_frame_count;
	int num_children;
	struct wlr_output_mode *tiled_mode;
	struct wl_list children; //wlr_output_group_child.link
	struct wl_list mirrors; //wlr_output_group_child.link
	struct wlr_drm_format_set cursor_formats;
	struct wlr_drm_format_set primary_formats;
	struct wl_event_source *ready;
	struct wl_list link;
	struct wlr_backend backend;
	struct wlr_output_cursor_size *cursor_sizes;
	size_t cursor_sizes_len;
};

struct wlr_output_group *wlr_output_group_create(void);
struct wlr_output_group *wlr_output_group_match_tile(struct wlr_output_group_tile_info *tile_info);
void wlr_output_group_add_tile(struct wlr_output_group *group, struct wlr_output *output,
	struct wlr_output_group_tile_info *tile_info);
void wlr_output_group_add_mirror(struct wlr_output_group *group, struct wlr_output *output);
void wlr_output_group_remove(struct wlr_output_group *group, struct wlr_output *output);
void wlr_output_group_ready(struct wlr_output_group *group);

#endif
