/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_INTERFACES_WLR_EXT_IMAGE_CAPTURE_SOURCE_V1_H
#define WLR_INTERFACES_WLR_EXT_IMAGE_CAPTURE_SOURCE_V1_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>

struct wlr_ext_image_copy_capture_frame_v1;

struct wlr_ext_image_capture_source_v1_interface {
	void (*schedule_frame)(struct wlr_ext_image_capture_source_v1 *source);
	void (*copy_frame)(struct wlr_ext_image_capture_source_v1 *source,
		struct wlr_ext_image_copy_capture_frame_v1 *dst_frame,
		struct wlr_ext_image_capture_source_v1_frame_event *frame_event);
};

void wlr_ext_image_capture_source_v1_init(struct wlr_ext_image_capture_source_v1 *source,
		const struct wlr_ext_image_capture_source_v1_interface *impl);
void wlr_ext_image_capture_source_v1_finish(struct wlr_ext_image_capture_source_v1 *source);
bool wlr_ext_image_capture_source_v1_create_resource(struct wlr_ext_image_capture_source_v1 *source,
	struct wl_client *client, uint32_t new_id);

#endif
