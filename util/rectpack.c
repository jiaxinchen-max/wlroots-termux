#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/rectpack.h>

struct wlr_rectpack_bandbuf {
	pixman_box32_t *data;
	size_t len;
	size_t cap;
};

static void bandbuf_init(struct wlr_rectpack_bandbuf *buf) {
	*buf = (struct wlr_rectpack_bandbuf){0};
}

static void bandbuf_finish(struct wlr_rectpack_bandbuf *buf) {
	free(buf->data);
}

static bool bandbuf_add(struct wlr_rectpack_bandbuf *buf, pixman_box32_t *band) {
	if (buf->len == buf->cap) {
		buf->cap = buf->cap == 0 ? 32 : buf->cap * 2;
		pixman_box32_t *data = realloc(buf->data, sizeof(*data) * buf->cap);
		if (data == NULL) {
			return false;
		}
		buf->data = data;
	}
	buf->data[buf->len++] = *band;
	return true;
}

static bool lines_overlap(int a1, int b1, int a2, int b2) {
	int max_a = a1 > a2 ? a1 : a2;
	int min_b = b1 < b2 ? b1 : b2;
	return min_b > max_a;
}

// Returns false if the constraint overlaps with the origin
static bool line_crop(int *a, int *b, int exclusive_a, int exclusive_b,
		int origin_a, int origin_b) {
	if (exclusive_a >= origin_b) {
		if (*b > exclusive_a) {
			*b = exclusive_a;
		}
	} else if (exclusive_b <= origin_a) {
		if (*a < exclusive_b) {
			*a = exclusive_b;
		}
	} else {
		return false;
	}
	return true;
}

// Returns false on memory allocation error
static bool grow_2d(const struct wlr_box *bounds, pixman_region32_t *exclusive,
		pixman_box32_t *target) {
	// The goal is to find the largest empty rectangle within the exclusive region such that it
	// would contain the target rectangle. To achieve this, we split the remaining empty space into
	// horizontal bands in such a way that they form two trapezoids (top and bottom), and then
	// iterate over pairs of bands from each trapezoid to find the largest rectangle.

	// Note: Pixman regions are stored as sorted "y-x-banded" arrays of rectangles. For
	// implementation details, see pixman-region.c.

	int n_exclusive_rects;
	pixman_box32_t *exclusive_rects = pixman_region32_rectangles(exclusive, &n_exclusive_rects);

	// Step 1: find the middle band, split the exclusive region in 3 subregions:
	// - above the target;
	// - vertically overlapping with the target;
	// - below the target.

	// The widest band, contains the target
	pixman_box32_t mid_band = (pixman_box32_t){
		.x1 = bounds->x,
		.y1 = bounds->y,
		.x2 = bounds->x + bounds->width,
		.y2 = bounds->y + bounds->height,
	};

	// Find exclusive rectangles which are above the target, crop the middle band from the top
	int above_rect_i = 0;
	for (; above_rect_i < n_exclusive_rects; above_rect_i++) {
		pixman_box32_t *rect = &exclusive_rects[above_rect_i];
		if (rect->y2 > target->y1) {
			break;
		}
		mid_band.y1 = rect->y2;
	}

	// Find exclusive rectangles which vertically overlap with the target, crop the middle band from
	// the other sides
	int below_rect_i = above_rect_i--;
	for (; below_rect_i < n_exclusive_rects; below_rect_i++) {
		pixman_box32_t *rect = &exclusive_rects[below_rect_i];
		if (rect->y1 >= target->y2) {
			mid_band.y2 = rect->y1;
			break;
		}

		// Invariant: no exclusive rectangle overlaps with the minimum box
		line_crop(&mid_band.x1, &mid_band.x2, rect->x1, rect->x2, target->x1, target->x2);
	}

	// The rest of the exclusive rectangles are below the target

	// Step 2: find the rest of the bands.

	bool ok = false;

	struct wlr_rectpack_bandbuf bandbuf;
	bandbuf_init(&bandbuf);


	// Find all "above" bands, moving up from the middle
	// Note: this includes the middle band itself
	if (!bandbuf_add(&bandbuf, &mid_band)) {
		goto end;
	}

	while (above_rect_i >= 0) {
		pixman_box32_t *rect = &exclusive_rects[above_rect_i];
		pixman_box32_t *last = &bandbuf.data[bandbuf.len - 1];

		// Invariant: a band farther from the middle one is horizontally contained by a band closer
		// to the middle one
		pixman_box32_t band = {
			.x1 = last->x1,
			.y1 = rect->y1,
			.x2 = last->x2,
			.y2 = rect->y2,
		};
		// Extend the last one up in case of free vertical space
		last->y1 = band.y2;

		// Process the x-band of exclusive rectangles
		do {
			if (!line_crop(&band.x1, &band.x2, rect->x1, rect->x2, target->x1, target->x2)) {
				// A rectangle is horizontally overlapping with the target; it's not possible to go
				// further
				goto above_done;
			} else if (above_rect_i-- == 0) {
				// All rectangles processed
				break;
			}
			rect = &exclusive_rects[above_rect_i];
		} while (rect->y1 == band.y1);

		if (band.x1 == last->x1 && band.x2 == last->x2) {
			// Horizontally equal to the last; extend that up instead
			last->y1 = band.y1;
		} else {
			if (!bandbuf_add(&bandbuf, &band)) {
				goto end;
			}
		}
	}
	// Extend the last one up in case of free vertical space
	bandbuf.data[bandbuf.len - 1].y1 = bounds->y;
above_done:;

	size_t split_i = bandbuf.len;

	// Find all "below" bands, moving down from the middle
	// Same logic applies

	if (!bandbuf_add(&bandbuf, &mid_band)) {
		goto end;
	}

	while (below_rect_i < n_exclusive_rects) {
		pixman_box32_t *rect = &exclusive_rects[below_rect_i];
		pixman_box32_t *last = &bandbuf.data[bandbuf.len - 1];

		pixman_box32_t band = {
			.x1 = last->x1,
			.y1 = rect->y1,
			.x2 = last->x2,
			.y2 = rect->y2,
		};
		last->y2 = band.y1;

		do {
			if (!line_crop(&band.x1, &band.x2, rect->x1, rect->x2, target->x1, target->x2)) {
				goto below_done;
			} else if (++below_rect_i == n_exclusive_rects) {
				break;
			}
			rect = &exclusive_rects[below_rect_i];
		} while (rect->y1 == band.y1);

		if (band.x1 == last->x1 && band.x2 == last->x2) {
			last->y2 = band.y2;
		} else {
			if (!bandbuf_add(&bandbuf, &band)) {
				goto end;
			}
		}
	}
	bandbuf.data[bandbuf.len - 1].y2 = bounds->y + bounds->height;
below_done:;

	// Step 3: find the largest rectangle within the empty bands. Between rectangles with the same
	// area, pick the one that uses the smaller bounds space better; i.e. pick a "more vertical"
	// rectangle within horizontal bounds and vice versa.

	bool bounds_horizontal = bounds->width > bounds->height;
	int best_area = (target->x2 - target->x1) * (target->y2 - target->y1);

	// Note: the (mid_band, mid_band) pair is checked too
	for (size_t above_i = 0; above_i < split_i; above_i++) {
		pixman_box32_t *above = &bandbuf.data[above_i];
		for (size_t below_i = split_i; below_i < bandbuf.len; below_i++) {
			pixman_box32_t *below = &bandbuf.data[below_i];

			pixman_box32_t curr = {
				.x1 = above->x1 > below->x1 ? above->x1 : below->x1,
				.y1 = above->y1,
				.x2 = above->x2 < below->x2 ? above->x2 : below->x2,
				.y2 = below->y2,
			};

			int width = curr.x2 - curr.x1, height = curr.y2 - curr.y1;
			int area = width * height;
			if (area > best_area || (area == best_area && bounds_horizontal != (width > height))) {
				*target = curr;
				best_area = area;
			}
		}
	}

	ok = true;

end:
	bandbuf_finish(&bandbuf);
	return ok;
}

bool wlr_rectpack_place(const struct wlr_box *bounds, pixman_region32_t *exclusive,
		const struct wlr_box *box, struct wlr_rectpack_rules *rules, struct wlr_box *out) {
	assert(!wlr_box_empty(box));

	if (bounds->width < box->width || bounds->height < box->height) {
		// Trivial case: the bounds are not big enough for the minimum box
		return false;
	}

	int n_exclusive_rects = 0;
	pixman_box32_t *exclusive_rects = NULL;
	if (exclusive != NULL) {
		exclusive_rects = pixman_region32_rectangles(exclusive, &n_exclusive_rects);
	}

	if (n_exclusive_rects == 0) {
		// Trivial case: the exclusive region is empty or ignored, just stretch to bounds as needed
		if (rules->grow_width) {
			out->x = bounds->x;
			out->width = bounds->width;
		} else {
			out->x = box->x;
			out->width = box->width;
		}
		if (rules->grow_height) {
			out->y = bounds->y;
			out->height = bounds->height;
		} else {
			out->y = box->y;
			out->height = box->height;
		}
		return true;
	}

	// Step 1: fit the minimum box within the exclusive region.

	// Instead of trying to fit a min_width×min_height rectangle, shrink the available region and
	// try to fit a 1×1 rectangle.
	int dwidth = box->width - 1;
	int dheight = box->height - 1;

	pixman_box32_t shrunk_bounds = {
		.x1 = bounds->x,
		.y1 = bounds->y,
		.x2 = bounds->x + bounds->width - dwidth,
		.y2 = bounds->y + bounds->height - dheight,
	};

	pixman_region32_t available;
	pixman_region32_init(&available);

	if (dwidth != 0 || dheight != 0) {
		pixman_box32_t *expanded_rects = calloc(n_exclusive_rects, sizeof(*expanded_rects));
		if (expanded_rects == NULL) {
			wlr_log(WLR_ERROR, "Allocation failed");
			pixman_region32_fini(&available);
			return false;
		}

		for (int i = 0; i < n_exclusive_rects; i++) {
			pixman_box32_t *rect = &exclusive_rects[i];
			expanded_rects[i] = (pixman_box32_t){
				.x1 = rect->x1 - dwidth,
				.y1 = rect->y1 - dheight,
				.x2 = rect->x2,
				.y2 = rect->y2,
			};
		}

		pixman_region32_t expanded;
		pixman_region32_init_rects(&expanded, expanded_rects, n_exclusive_rects);
		pixman_region32_inverse(&available, &expanded, &shrunk_bounds);
		pixman_region32_fini(&expanded);

		free(expanded_rects);
	} else {
		// Fast path: the minimum box is already 1×1
		pixman_region32_inverse(&available, exclusive, &shrunk_bounds);
	}

	int n_available_rects;
	pixman_box32_t *available_rects = pixman_region32_rectangles(&available, &n_available_rects);
	if (n_available_rects == 0) {
		// Not enough free space within the exclusive region for the minimum box
		pixman_region32_fini(&available);
		return false;
	}

	// Find the position closest to the desired one
	int best_x = box->x, best_y = box->y;
	int best_dist_sq = INT_MAX;
	for (int i = 0; i < n_available_rects; i++) {
		pixman_box32_t *rect = &available_rects[i];
		int clamped_x = box->x < rect->x1 ? rect->x1 :
			box->x >= rect->x2 ? rect->x2 - 1 : box->x;
		int clamped_y = box->y < rect->y1 ? rect->y1 :
			box->y >= rect->y2 ? rect->y2 - 1 : box->y;

		int dx = clamped_x - box->x, dy = clamped_y - box->y;
		int dist_sq = dx * dx + dy * dy;
		if (dist_sq < best_dist_sq) {
			best_dist_sq = dist_sq;
			best_x = clamped_x;
			best_y = clamped_y;
		}

		if (best_dist_sq == 0) {
			break;
		}
	}
	pixman_region32_fini(&available);

	// Step 2: grow the box as requested.

	pixman_box32_t result = {
		.x1 = best_x,
		.y1 = best_y,
		.x2 = best_x + box->width,
		.y2 = best_y + box->height,
	};

	if (rules->grow_width && rules->grow_height) {
		if (!grow_2d(bounds, exclusive, &result)) {
			return false;
		}
	} else if (rules->grow_width) {
		// Stretch and then crop
		int o1 = result.x1, o2 = result.x2;
		result.x1 = bounds->x;
		result.x2 = bounds->x + bounds->width;

		for (int i = 0; i < n_exclusive_rects; i++) {
			pixman_box32_t *rect = &exclusive_rects[i];
			if (lines_overlap(result.y1, result.y2, rect->y1, rect->y2)) {
				// Invariant: no exclusive rectangle overlaps with the minimum box
				line_crop(&result.x1, &result.x2, rect->x1, rect->x2, o1, o2);
			}
		}
	} else if (rules->grow_height) {
		// Same as width
		int o1 = result.y1, o2 = result.y2;
		result.y1 = bounds->y;
		result.y2 = bounds->y + bounds->height;

		for (int i = 0; i < n_exclusive_rects; i++) {
			pixman_box32_t *rect = &exclusive_rects[i];
			if (lines_overlap(result.x1, result.x2, rect->x1, rect->x2)) {
				// Invariant: no exclusive rectangle overlaps with the minimum box
				line_crop(&result.y1, &result.y2, rect->y1, rect->y2, o1, o2);
			}
		}
	}

	*out = (struct wlr_box){
		.x = result.x1,
		.y = result.y1,
		.width = result.x2 - result.x1,
		.height = result.y2 - result.y1,
	};
	return true;
}

bool wlr_rectpack_place_wlr_layer_surface_v1(const struct wlr_box *bounds,
		pixman_region32_t *exclusive, struct wlr_layer_surface_v1 *surface, struct wlr_box *out) {
	struct wlr_layer_surface_v1_state *state = &surface->current;
	uint32_t anchor = state->anchor;

	int m_top = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP ? state->margin.top : 0;
	int m_bottom = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM ? state->margin.bottom : 0;
	int m_left = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT ? state->margin.left : 0;
	int m_right = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT ? state->margin.right : 0;

	int m_horiz = m_left + m_right;
	int m_verti = m_top + m_bottom;

	enum wlr_edges exclusive_edge = wlr_layer_surface_v1_get_exclusive_edge(surface);
	int full_exclusive_zone = state->exclusive_zone;

	switch (exclusive_edge) {
	case WLR_EDGE_LEFT:
		full_exclusive_zone += m_left;
		break;
	case WLR_EDGE_RIGHT:
		full_exclusive_zone += m_right;
		break;
	case WLR_EDGE_TOP:
		full_exclusive_zone += m_top;
		break;
	case WLR_EDGE_BOTTOM:
		full_exclusive_zone += m_bottom;
		break;
	case WLR_EDGE_NONE:
		break;
	}

	int desired_width = (int)state->desired_width, desired_height = (int)state->desired_height;
	bool grow_width = desired_width == 0, grow_height = desired_height == 0;

	int min_width = (grow_width ? 1 : desired_width) + m_horiz;
	int min_height = (grow_height ? 1 : desired_height) + m_verti;

	if (min_width < 1) {
		min_width = 1;
	}
	if (min_height < 1) {
		min_height = 1;
	}

	switch (exclusive_edge) {
	case WLR_EDGE_LEFT:
	case WLR_EDGE_RIGHT:
		if (min_width < full_exclusive_zone) {
			min_width = full_exclusive_zone;
		}
		break;
	case WLR_EDGE_TOP:
	case WLR_EDGE_BOTTOM:
		if (min_height < full_exclusive_zone) {
			min_height = full_exclusive_zone;
		}
		break;
	case WLR_EDGE_NONE:
		break;
	}

	uint32_t edges = anchor;
	if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) == (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
		edges &= ~(WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	}
	if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) == (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
		edges &= ~(WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
	}

	struct wlr_box box = {
		.x = bounds->x,
		.y = bounds->y,
		.width = min_width,
		.height = min_height,
	};

	if ((anchor & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) == WLR_EDGE_RIGHT) {
		box.x += bounds->width - box.width;
	} else if ((anchor & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != WLR_EDGE_LEFT) {
		box.x += bounds->width / 2 - box.width / 2;
	}
	if ((anchor & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) == WLR_EDGE_BOTTOM) {
		box.y += bounds->height - box.height;
	} else if ((anchor & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != WLR_EDGE_TOP) {
		box.y += bounds->height / 2 - box.height / 2;
	}

	struct wlr_rectpack_rules rules = {
		.grow_width = grow_width,
		.grow_height = grow_height,
	};

	if (!wlr_rectpack_place(bounds, state->exclusive_zone >= 0 ? exclusive : NULL,
			&box, &rules, out)) {
		return false;
	}

	if (exclusive_edge != WLR_EDGE_NONE) {
		struct wlr_box exclusive_box = *out;
		switch (exclusive_edge) {
		case WLR_EDGE_RIGHT:
			exclusive_box.x += out->width - full_exclusive_zone;
			// Fallthrough
		case WLR_EDGE_LEFT:
			exclusive_box.width = full_exclusive_zone;
			break;
		case WLR_EDGE_BOTTOM:
			exclusive_box.y += out->height - full_exclusive_zone;
			// Fallthrough
		case WLR_EDGE_TOP:
			exclusive_box.height = full_exclusive_zone;
			break;
		case WLR_EDGE_NONE:
			abort(); // Unreachable
		}

		struct wlr_box intersection;
		if (wlr_box_intersection(&intersection, &exclusive_box, bounds)) {
			pixman_region32_union_rect(exclusive, exclusive, intersection.x,
				intersection.y, (unsigned int)intersection.width,
				(unsigned int)intersection.height);
		}
	}

	return true;
}
