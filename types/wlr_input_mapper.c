#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_input_mapper.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

static void constraint_detach_output(struct wlr_input_constraint *constraint) {
	constraint->output = NULL;
	wl_list_remove(&constraint->output_destroy.link);
	wl_list_init(&constraint->output_destroy.link);
}

static void constraint_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_constraint *constraint = wl_container_of(listener, constraint, output_destroy);
	constraint_detach_output(constraint);
}

static void constraint_init(struct wlr_input_constraint *constraint) {
	*constraint = (struct wlr_input_constraint){0};

	constraint->output_destroy.notify = constraint_handle_output_destroy;
	wl_list_init(&constraint->output_destroy.link);
}

static void constraint_finish(struct wlr_input_constraint *constraint) {
	wl_list_remove(&constraint->output_destroy.link);
}

static void mapping_destroy(struct wlr_input_mapping *mapping) {
	constraint_finish(&mapping->constraint);
	wlr_addon_finish(&mapping->addon);
	wl_list_remove(&mapping->link);
	free(mapping);
}

static void device_addon_destroy(struct wlr_addon *addon) {
	struct wlr_input_mapping *mapping = wl_container_of(addon, mapping, addon);
	mapping_destroy(mapping);
}

static const struct wlr_addon_interface device_addon_impl = {
	.name = "wlr_input_mapping",
	.destroy = device_addon_destroy,
};

static struct wlr_input_mapping *mapping_create(struct wlr_input_mapper *mapper,
		struct wlr_input_device *device) {
	struct wlr_input_mapping *mapping = calloc(1, sizeof(*mapping));
	if (mapping == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	constraint_init(&mapping->constraint);
	wlr_addon_init(&mapping->addon, &device->addons, mapper, &device_addon_impl);
	wl_list_insert(&mapper->mappings, &mapping->link);

	return mapping;
}

static void detach_output_layout(struct wlr_input_mapper *mapper) {
	mapper->layout = NULL;
	wl_list_remove(&mapper->layout_destroy.link);
	wl_list_init(&mapper->layout_destroy.link);
}

static void handle_layout_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_mapper *mapper = wl_container_of(listener, mapper, layout_destroy);
	detach_output_layout(mapper);
}

static struct wlr_input_constraint *get_constraint(struct wlr_input_mapper *mapper,
		struct wlr_input_device *device, bool create) {
	if (device != NULL) {
		struct wlr_input_mapping *mapping = NULL;
		struct wlr_addon *addon = wlr_addon_find(&device->addons, mapper, &device_addon_impl);
		if (addon != NULL) {
			mapping = wl_container_of(addon, mapping, addon);
		}

		if (mapping != NULL) {
			return &mapping->constraint;
		}

		if (create) {
			mapping = mapping_create(mapper, device);
			if (mapping == NULL) {
				return NULL;
			}
		}
	}

	return &mapper->global;
}

static void get_constraint_box(struct wlr_input_mapper *mapper, struct wlr_input_device *device,
		struct wlr_box *box) {
	*box = (struct wlr_box){0};

	struct wlr_input_constraint *constraint = get_constraint(mapper, device, false);
	if (!wlr_box_empty(&constraint->box)) {
		*box = constraint->box;
	} else if (mapper->layout != NULL && constraint->output != NULL) {
		wlr_output_layout_get_box(mapper->layout, constraint->output, box);
		assert(!wlr_box_empty(box));
	}
}

struct wlr_input_mapper *wlr_input_mapper_create(void) {
	struct wlr_input_mapper *mapper = calloc(1, sizeof(*mapper));
	if (mapper == NULL) {
		return NULL;
	}

	constraint_init(&mapper->global);

	wl_list_init(&mapper->mappings);
	wl_signal_init(&mapper->events.destroy);

	mapper->layout_destroy.notify = handle_layout_destroy;
	wl_list_init(&mapper->layout_destroy.link);

	return mapper;
}

void wlr_input_mapper_destroy(struct wlr_input_mapper *mapper) {
	if (mapper == NULL) {
		return;
	}

	wl_signal_emit_mutable(&mapper->events.destroy, NULL);

	struct wlr_input_mapping *mapping, *tmp;
	wl_list_for_each_safe(mapping, tmp, &mapper->mappings, link) {
		mapping_destroy(mapping);
	}

	constraint_finish(&mapper->global);

	wl_list_remove(&mapper->layout_destroy.link);
	free(mapper);
}

void wlr_input_mapper_attach_output_layout(struct wlr_input_mapper *mapper,
		struct wlr_output_layout *layout) {
	detach_output_layout(mapper);
	mapper->layout = layout;
	if (layout != NULL) {
		wl_signal_add(&layout->events.destroy, &mapper->layout_destroy);
	}
}

void wlr_input_mapper_absolute_to_layout(struct wlr_input_mapper *mapper,
		struct wlr_input_device *device, double x, double y, double *lx, double *ly) {
	struct wlr_box box;
	get_constraint_box(mapper, device, &box);
	if (wlr_box_empty(&box) && mapper->layout != NULL) {
		wlr_output_layout_get_box(mapper->layout, NULL, &box);
	}

	// At this point, if no matching constraint was found and the layout is NULL or empty, box is
	// filled with zeroes.
	*lx = x * box.width + box.x;
	*ly = y * box.height + box.y;
}

void wlr_input_mapper_closest_point(struct wlr_input_mapper *mapper,
		struct wlr_input_device *device, double lx, double ly,
		double *closest_lx, double *closest_ly) {
	struct wlr_box box;
	get_constraint_box(mapper, device, &box);
	if (!wlr_box_empty(&box)) {
		wlr_box_closest_point(&box, lx, ly, closest_lx, closest_ly);
	} else if (mapper->layout != NULL) {
		wlr_output_layout_closest_point(mapper->layout, NULL, lx, ly, closest_lx, closest_ly);
	} else {
		*closest_lx = lx;
		*closest_ly = ly;
	}
}

bool wlr_input_mapper_map_to_output(struct wlr_input_mapper *mapper,
		struct wlr_input_device *device, struct wlr_output *output) {
	struct wlr_input_constraint *constraint = get_constraint(mapper, device, true);
	if (constraint == NULL) {
		return false;
	}

	constraint_detach_output(constraint);
	constraint->output = output;
	if (output != NULL) {
		wl_signal_add(&output->events.destroy, &constraint->output_destroy);
	}

	return true;
}

bool wlr_input_mapper_map_to_box(struct wlr_input_mapper *mapper, struct wlr_input_device *device,
		const struct wlr_box *box) {
	struct wlr_input_constraint *constraint = get_constraint(mapper, device, true);
	if (constraint == NULL) {
		return false;
	}

	if (!wlr_box_empty(box)) {
		constraint->box = *box;
	} else {
		constraint->box = (struct wlr_box){0};
	}

	return true;
}
