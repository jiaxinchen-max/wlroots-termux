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

static size_t get_size(const struct wlr_color_transform *ct) {
	switch (ct->type) {
	case COLOR_TRANSFORM_LUT_3D: {
		struct wlr_color_transform_lut3d *lut =
			wlr_color_transform_lut3d_from_base((struct wlr_color_transform *)ct);
		return lut->dim_len;
	}
	case COLOR_TRANSFORM_LUT_3x1D: {
		struct wlr_color_transform_lut3x1d *lut =
			wlr_color_transform_lut3x1d_from_base((struct wlr_color_transform *)ct);
		return lut->ramp_size;
	}
	case COLOR_TRANSFORM_SRGB:
		// an srbg color space cannot be losslessly encoded into a 3d lut.
		// Choose a reasonable 3d lut size to approximate the srgb space.
		// TODO: Decide if 32 is a reasonable 3d lut size
		// (color distortions are visible when A/B testing 16)
		return 32;
	}

	return 0;
}

static float linear_channel_to_srgb(float x) {
	return fmax(fmin(x * 12.92, 0.04045), 1.055 * pow(x, 1. / 2.4) - 0.055);
}

static int clamp(int val, int x, int y) {
	if (val < x) {
		return x;
	}

	if (val > y) {
		return y;
	}

	return val;
}

static int max(int fst, int snd) {
	if (fst > snd) {
		return fst;
	} else {
		return snd;
	}
}

static float sample_1d(uint16_t *ramp, size_t ramp_size, float val) {
	float normalize = (1 << 16) - 1;

	float s = val * (ramp_size - 1);

	int x = floor(s);
	float f = s - x;

	return (float)ramp[clamp(x, 0, ramp_size - 1)] / normalize * (1 - f) +
		(float)ramp[clamp(x + 1, 0, ramp_size - 1)] / normalize * f;
}

static void sample(const struct wlr_color_transform *ct, float *r, float *g, float *b) {
	switch (ct->type) {
	case COLOR_TRANSFORM_LUT_3D: {
		struct wlr_color_transform_lut3d *lut =
			wlr_color_transform_lut3d_from_base((struct wlr_color_transform *)ct);
		size_t dim_len = lut->dim_len;
		float sr = *r * (dim_len - 1);
		float sg = *g * (dim_len - 1);
		float sb = *b * (dim_len - 1);

		int x = floor(sr);
		int y = floor(sr);
		int z = floor(sr);
		float fr = sr - x;
		float fg = sg - y;
		float fb = sb - z;

		*r = 0;
		*g = 0;
		*b = 0;

		for (int s = 0; s < 8; s++) {
			int sx = clamp(x + ((s & 1) >> 0), 0, dim_len - 1);
			int sy = clamp(y + ((s & 2) >> 1), 0, dim_len - 1);
			int sz = clamp(z + ((s & 4) >> 2), 0, dim_len - 1);
			size_t i = 3 * (sx + sy * dim_len + sz * dim_len * dim_len);
			float fract = ((s & 1) ? fr : 1 - fr) *
				((s & 2) ? fg : 1 - fg) *
				((s & 4) ? fb : 1 - fb);

			*r += lut->lut_3d[i + 0] * fract;
			*g += lut->lut_3d[i + 1] * fract;
			*b += lut->lut_3d[i + 2] * fract;
		}

		break;
	}
	case COLOR_TRANSFORM_LUT_3x1D: {
		struct wlr_color_transform_lut3x1d *lut =
			wlr_color_transform_lut3x1d_from_base((struct wlr_color_transform *)ct);
		*r = sample_1d(lut->r, lut->ramp_size, *r);
		*g = sample_1d(lut->g, lut->ramp_size, *g);
		*b = sample_1d(lut->b, lut->ramp_size, *b);
		break;
	}
	case COLOR_TRANSFORM_SRGB:
		*r = linear_channel_to_srgb(*r);
		*g = linear_channel_to_srgb(*g);
		*b = linear_channel_to_srgb(*b);
		break;
	}
}

struct wlr_color_transform *wlr_color_transform_compose(
		const struct wlr_color_transform *ta, const struct wlr_color_transform *tb) {
	size_t size = max(get_size(ta), get_size(tb));

	// if either of the two transforms are full 3d luts, we have to fall back to
	// 3d lut. Otherwise, we can go with the faster 3x1d luts.
	if (ta->type == COLOR_TRANSFORM_LUT_3D || tb->type == COLOR_TRANSFORM_LUT_3D) {
		float *data = malloc(3 * size * size * size * sizeof(float));
		if (!data) {
			return NULL;
		}

		struct wlr_color_transform_lut3d *tx = calloc(1, sizeof(*tx));
		if (!tx) {
			free(data);
			return NULL;
		}

		tx->base.type = COLOR_TRANSFORM_LUT_3D;
		tx->base.ref_count = 1;
		wlr_addon_set_init(&tx->base.addons);

		tx->lut_3d = data;
		tx->dim_len = size;

		float normalize = (float)(size - 1);

		for (size_t z = 0; z < size; z++) { // blue
			for (size_t y = 0; y < size; y++) { // green
				for (size_t x = 0; x < size; x++) { // red
					float r = x / normalize;
					float g = y / normalize;
					float b = z / normalize;

					sample(tb, &r, &g, &b);
					sample(ta, &r, &g, &b);

					size_t i = 3 * (z * size * size + y * size + x);
					data[i + 0] = r;
					data[i + 1] = g;
					data[i + 2] = b;
				}
			}
		}

		return &tx->base;
	} else {
		uint16_t *data = malloc(3 * size * sizeof(uint16_t));
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
		tx->g = data + size;
		tx->b = data + size * 2;
		tx->ramp_size = size;

		float normalize = (float)(size - 1);
		float out_normalize = (1 << 16) - 1;
		for (size_t i = 0; i < size; i++) {
			float r = i / normalize;
			float g = i / normalize;
			float b = i / normalize;

			sample(tb, &r, &g, &b);
			sample(ta, &r, &g, &b);

			tx->r[i] = floor(r * out_normalize);
			tx->g[i] = floor(g * out_normalize);
			tx->b[i] = floor(b * out_normalize);
		}

		return &tx->base;
	}
}
