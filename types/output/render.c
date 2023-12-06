#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/interface.h>
#include <wlr/render/swapchain.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "backend/backend.h"
#include "render/allocator/allocator.h"
#include "render/drm_format_set.h"
#include "render/wlr_renderer.h"
#include "render/pixel_format.h"
#include "types/wlr_output.h"

bool wlr_output_init_render(struct wlr_output *output,
		struct wlr_allocator *allocator, struct wlr_renderer *renderer) {
	assert(allocator != NULL && renderer != NULL);

	uint32_t backend_caps = backend_get_buffer_caps(output->backend);
	uint32_t renderer_caps = renderer->render_buffer_caps;

	if (!(backend_caps & allocator->buffer_caps)) {
		wlr_log(WLR_ERROR, "output backend and allocator buffer capabilities "
			"don't match");
		return false;
	} else if (!(renderer_caps & allocator->buffer_caps)) {
		wlr_log(WLR_ERROR, "renderer and allocator buffer capabilities "
			"don't match");
		return false;
	}

	wlr_swapchain_destroy(output->swapchain);
	output->swapchain = NULL;

	wlr_swapchain_destroy(output->cursor_swapchain);
	output->cursor_swapchain = NULL;

	output->allocator = allocator;
	output->renderer = renderer;

	return true;
}

void wlr_output_lock_attach_render(struct wlr_output *output, bool lock) {
	if (lock) {
		++output->attach_render_locks;
	} else {
		assert(output->attach_render_locks > 0);
		--output->attach_render_locks;
	}
	wlr_log(WLR_DEBUG, "%s direct scan-out on output '%s' (locks: %d)",
		lock ? "Disabling" : "Enabling", output->name,
		output->attach_render_locks);
}

bool output_pick_format(struct wlr_output *output,
		const struct wlr_drm_format_set *display_formats,
		struct wlr_drm_format *format, uint32_t fmt) {
	struct wlr_renderer *renderer = output->renderer;
	struct wlr_allocator *allocator = output->allocator;
	assert(renderer != NULL && allocator != NULL);

	const struct wlr_drm_format_set *render_formats =
		wlr_renderer_get_render_formats(renderer);
	if (render_formats == NULL) {
		wlr_log(WLR_ERROR, "Failed to get render formats");
		return false;
	}

	const struct wlr_drm_format *render_format =
		wlr_drm_format_set_get(render_formats, fmt);
	if (render_format == NULL) {
		wlr_log(WLR_DEBUG, "Renderer doesn't support format 0x%"PRIX32, fmt);
		return false;
	}

	if (display_formats != NULL) {
		const struct wlr_drm_format *display_format =
			wlr_drm_format_set_get(display_formats, fmt);
		if (display_format == NULL) {
			wlr_log(WLR_DEBUG, "Output doesn't support format 0x%"PRIX32, fmt);
			return false;
		}
		if (!wlr_drm_format_intersect(format, display_format, render_format)) {
			wlr_log(WLR_DEBUG, "Failed to intersect display and render "
				"modifiers for format 0x%"PRIX32 " on output %s",
				fmt, output->name);
			return false;
		}
	} else {
		// The output can display any format
		if (!wlr_drm_format_copy(format, render_format)) {
			return false;
		}
	}

	if (format->len == 0) {
		wlr_drm_format_finish(format);
		wlr_log(WLR_DEBUG, "Failed to pick output format");
		return false;
	}

	return true;
}

struct wlr_render_pass *wlr_output_begin_render_pass(struct wlr_output *output,
		struct wlr_output_state *state, struct wlr_buffer_pass_options *render_options) {
	if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
		return NULL;
	}

	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_renderer *renderer = output->renderer;
	assert(renderer != NULL);
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buffer, render_options);
	if (pass == NULL) {
		return NULL;
	}

	wlr_output_state_set_buffer(state, buffer);
	wlr_buffer_unlock(buffer);
	return pass;
}
