/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_TERMUXDC_H
#define WLR_BACKEND_TERMUXDC_H

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

/**
 * Creates a Termux:Display client backend, and connection to the Termux:Display server.
 * A Termux:Display client backend has no outputs or inputs by default.
 */
struct wlr_backend *wlr_termuxdc_backend_create(struct wl_event_loop *loop);
/**
 * Create a new Termux:Display client output.
 *
 * Will use Termux:Display server to connect to SurfaceView, the buffers presented
 * on the output is displayed to the SurfaceView.
 */
struct wlr_output *wlr_termuxdc_output_create(struct wlr_backend *backend);

bool wlr_backend_is_termuxdc(struct wlr_backend *backend);
bool wlr_output_is_termuxdc(struct wlr_output *output);

#endif
