/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_INPUT_MAPPER_H
#define WLR_TYPES_WLR_INPUT_MAPPER_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

struct wlr_input_device;

struct wlr_input_constraint {
	struct wlr_output *output; // NULL if unset
	struct wlr_box box; // Empty if unset

	// private state

	struct wl_listener output_destroy;
};

/**
* A helper for converting absolute coordinates received from input devices to layout-local
* coordinates and applyingcoordinate constraints.
 *
 * The constraints precendence is as follows:
 * 1) Device-specific box
 * 2) Device-specific output
 * 3) Global box
 * 4) Global output
 *
 * If no output layout is attached to the input mapper, all output constraints are ignored.
 */
struct wlr_input_mapper {
	struct wlr_output_layout *layout;
	struct wlr_input_constraint global;

	struct wl_list mappings; // wlr_input_mapping.link

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	struct wl_listener layout_destroy;
};

struct wlr_input_mapping {
	struct wlr_input_constraint constraint;
	struct wl_list link; // wlr_input_mapper.mappings

	// private state

	struct wlr_addon addon; // wlr_input_device.addons
};

struct wlr_input_mapper *wlr_input_mapper_create(void);

void wlr_input_mapper_destroy(struct wlr_input_mapper *mapper);

/**
 * Attach an output layout to the input mapper. This detaches the previous output layout, if any.
 *
 * layout may be NULL.
 */
void wlr_input_mapper_attach_output_layout(struct wlr_input_mapper *mapper,
	struct wlr_output_layout *layout);

/**
 * Convert absolute coordinates in 0..1 range to layout-local coordinates.
 *
 * If device is not NULL, its constraints are used, if any. If no matching constraint is found, the
 * absolute coordinates are mapped to the entire layout, unless none is attached, in which case lx
 * and ly are set to 0.
 */
void wlr_input_mapper_absolute_to_layout(struct wlr_input_mapper *mapper,
	struct wlr_input_device *device, double x, double y, double *lx, double *ly);

/**
 * Get the closest point satisfying constraints from the given point.
 *
 * If device is not NULL, its constraints are used, if any. If no matching constraint is found, get
 * the closest point from the layout, unless it's not attached, in which case the original
 * coordinates are returned.
 */
void wlr_input_mapper_closest_point(struct wlr_input_mapper *mapper,
	struct wlr_input_device *device, double lx, double ly, double *closest_lx, double *closest_ly);

/**
 * Map device to output. If device is NULL, sets the default output constraint. If output is NULL,
 * the output constraint is reset.
 *
 * When the output is destroyed, the output constraint is reset.
 *
 * Returns true on success, false on memory allocation error.
 */
bool wlr_input_mapper_map_to_output(struct wlr_input_mapper *mapper,
	struct wlr_input_device *device, struct wlr_output *output);

/**
 * Map device to box. If device is NULL, sets the default box constraint. If box is NULL or empty,
 * the box constraint is reset.
 *
 * Returns true on success, false on memory allocation error.
 */
bool wlr_input_mapper_map_to_box(struct wlr_input_mapper *mapper, struct wlr_input_device *device,
	const struct wlr_box *box);

#endif
