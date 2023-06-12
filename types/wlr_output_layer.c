#include <assert.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/util/log.h>

struct wlr_output_layer *wlr_output_layer_create(struct wlr_output *output) {
	struct wlr_output_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		return NULL;
	}

	wl_list_insert(&output->layers, &layer->link);
	wlr_addon_set_init(&layer->addons);
	wl_signal_init(&layer->events.feedback);

	return layer;
}

void wlr_output_layer_destroy(struct wlr_output_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wlr_addon_set_finish(&layer->addons);
	wl_list_remove(&layer->link);
	free(layer);
}

bool wlr_output_state_is_layer_enabled(const struct wlr_output_state *state,
		struct wlr_output_layer *layer) {
	assert(state->committed & WLR_OUTPUT_STATE_LAYERS);

	for (size_t i = 0; i < state->layers_len; i++) {
		if (state->layers[i].layer == layer) {
			return true;
		}
	}

	return false;
}
