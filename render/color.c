#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/color.h>
#include "render/color.h"

struct wlr_color_transform *wlr_color_transform_init_srgb(void) {
	struct wlr_color_transform *tx = calloc(1, sizeof(struct wlr_color_transform));
	if (!tx) {
		return NULL;
	}
	tx->type = COLOR_TRANSFORM_SRGB;
	tx->ref_count = 1;
	wlr_addon_set_init(&tx->addons);
	return tx;
}

static void color_transform_destroy(struct wlr_color_transform *tr) {
	switch (tr->type) {
	case COLOR_TRANSFORM_SRGB:
		break;
	case COLOR_TRANSFORM_LUT_3D:;
		struct wlr_color_transform_lut3d *lut3d =
			wlr_color_transform_lut3d_from_base(tr);
		free(lut3d->lut_3d);
		break;
	case COLOR_TRANSFORM_LUT_3x1D:;
		struct wlr_color_transform_lut3x1d *lut3x1d =
			wlr_color_transform_lut3x1d_from_base(tr);
		free(lut3x1d->r);
		break;
	}
	wlr_addon_set_finish(&tr->addons);
	free(tr);
}

struct wlr_color_transform *wlr_color_transform_ref(struct wlr_color_transform *tr) {
	tr->ref_count += 1;
	return tr;
}

void wlr_color_transform_unref(struct wlr_color_transform *tr) {
	if (!tr) {
		return;
	}
	assert(tr->ref_count > 0);
	tr->ref_count -= 1;
	if (tr->ref_count == 0) {
		color_transform_destroy(tr);
	}
}

struct wlr_color_transform *wlr_color_transform_create_from_gamma_lut(
		size_t ramp_size, const uint16_t *r, const uint16_t *g, const uint16_t *b) {
	uint16_t *data = malloc(3 * ramp_size * sizeof(uint16_t));
	if (!data) {
		return NULL;
	}

	struct wlr_color_transform_lut3x1d *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		free(data);
		return NULL;
	}

	tx->base.type = COLOR_TRANSFORM_LUT_3x1D;
	tx->base.ref_count = 1;
	wlr_addon_set_init(&tx->base.addons);

	tx->r = data;
	tx->g = data + ramp_size;
	tx->b = data + ramp_size * 2;
	tx->ramp_size = ramp_size;

	memcpy(tx->r, r, ramp_size * sizeof(uint16_t));
	memcpy(tx->g, g, ramp_size * sizeof(uint16_t));
	memcpy(tx->b, b, ramp_size * sizeof(uint16_t));

	return &tx->base;
}

struct wlr_color_transform_lut3d *wlr_color_transform_lut3d_from_base(
		struct wlr_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_LUT_3D);
	struct wlr_color_transform_lut3d *lut3d = wl_container_of(tr, lut3d, base);
	return lut3d;
}

struct wlr_color_transform_lut3x1d *wlr_color_transform_lut3x1d_from_base(
		struct wlr_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_LUT_3x1D);
	struct wlr_color_transform_lut3x1d *lut = wl_container_of(tr, lut, base);
	return lut;
}
