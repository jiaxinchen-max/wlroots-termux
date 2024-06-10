/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_PIXMAN_H
#define WLR_RENDER_PIXMAN_H

#include <pixman.h>
#include <wlr/render/wlr_renderer.h>

struct wlr_renderer *wlr_pixman_renderer_create(void);
struct wlr_renderer *wlr_pixman_renderer_create_with_drm_fd(int drm_fd);
bool wlr_renderer_is_pixman(struct wlr_renderer *wlr_renderer);
bool wlr_texture_is_pixman(struct wlr_texture *texture);

pixman_image_t *wlr_pixman_renderer_get_buffer_image(
    struct wlr_renderer *wlr_renderer, struct wlr_buffer *wlr_buffer);
pixman_image_t *wlr_pixman_texture_get_image(struct wlr_texture *wlr_texture);

#endif
