#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_surface_invalidation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include "surface-invalidation-v1-protocol.h"

#define SURFACE_INVALIDATION_MANAGER_VERSION 1

struct wlr_surface_invalidation_v1_configure {
	struct wl_list link; // struct wlr_surface_invalidation_v1.configures
	uint32_t serial;
	bool configured;
};

struct wlr_surface_invalidation_v1 {
	struct wl_resource *resource;
	struct wl_list configures; // struct wlr_surface_invalidation_v1_configure.link
	bool inert;
	struct wlr_addon addon;
};

static void surface_invalidation_v1_configure_destroy(
		struct wlr_surface_invalidation_v1_configure *configure) {
	wl_list_remove(&configure->link);
	free(configure);
}

static const struct wp_surface_invalidation_v1_interface surface_inval_impl;

static struct wlr_surface_invalidation_v1 *surface_invalidation_v1_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_surface_invalidation_v1_interface,
		&surface_inval_impl));
	return wl_resource_get_user_data(resource);
}

static void surface_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_surface_invalidation_v1 *surface =
		surface_invalidation_v1_from_resource(resource);
	if (!surface->inert) {
		wlr_addon_finish(&surface->addon);
	}

	struct wlr_surface_invalidation_v1_configure *configure, *tmp_configure;
	wl_list_for_each_safe(configure, tmp_configure, &surface->configures, link) {
		surface_invalidation_v1_configure_destroy(configure);
	}

	free(surface);
}

static void surface_invalidation_handle_ack(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial) {
	struct wlr_surface_invalidation_v1 *surface =
		surface_invalidation_v1_from_resource(resource);

	// First find the ack'ed configure
	bool found = false;
	struct wlr_surface_invalidation_v1_configure *configure, *tmp_configure;
	wl_list_for_each(configure, &surface->configures, link) {
		if (configure->serial == serial) {
			found = true;
			break;
		}
	}
	if (!found) {
		/*
		 TODO: What do we do here?
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
			"wrong configure serial: %" PRIu32, serial);
		*/
		return;
	}

	configure->configured = true;

	// Then remove old configures from the list
	wl_list_for_each_safe(configure, tmp_configure, &surface->configures, link) {
		if (configure->serial == serial) {
			break;
		}
		surface_invalidation_v1_configure_destroy(configure);
	}
}

static void destroy_resource(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_surface_invalidation_v1_interface surface_inval_impl = {
	.destroy = destroy_resource,
	.ack = surface_invalidation_handle_ack,
};

static void surface_addon_handle_destroy(struct wlr_addon *addon) {
	struct wlr_surface_invalidation_v1 *surface = wl_container_of(addon, surface, addon);
	wlr_addon_finish(&surface->addon);
	surface->inert = true;
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "surface_invalidation_v1",
	.destroy = surface_addon_handle_destroy,
};

static const struct wp_surface_invalidation_manager_v1_interface manager_impl;

static struct wlr_surface_invalidation_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_surface_invalidation_manager_v1_interface,
		&manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_get_surface_invalidation(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);
	struct wlr_surface_invalidation_manager_v1 *manager = manager_from_resource(resource);

	struct wlr_surface_invalidation_v1 *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wl_client_post_no_memory(client);
		return;
	}

	surface->resource = wl_resource_create(client, &wp_surface_invalidation_v1_interface,
		wl_global_get_version(manager->global), id);
	if (!surface->resource) {
		wl_client_post_no_memory(client);
		free(surface);
		return;
	}

	wl_list_init(&surface->configures);
	wlr_addon_init(&surface->addon, &wlr_surface->addons, manager, &surface_addon_impl);

	wl_resource_set_implementation(surface->resource,
		&surface_inval_impl, surface, surface_handle_resource_destroy);
}

static const struct wp_surface_invalidation_manager_v1_interface manager_impl = {
	.destroy = destroy_resource,
	.get_surface_invalidation = manager_handle_get_surface_invalidation,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_surface_invalidation_manager_v1 *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&wp_surface_invalidation_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_surface_invalidation_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, NULL);
	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
}

struct wlr_surface_invalidation_manager_v1 *wlr_surface_invalidation_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= SURFACE_INVALIDATION_MANAGER_VERSION);

	struct wlr_surface_invalidation_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	manager->global = wl_global_create(display, &wp_surface_invalidation_manager_v1_interface,
		version, manager, manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	wl_signal_init(&manager->events.destroy);

	return manager;
}

bool wlr_surface_invalidation_manager_v1_invalidate(
		const struct wlr_surface_invalidation_manager_v1 *manager,
		struct wlr_surface *wlr_surface) {
	struct wlr_addon *addon = wlr_addon_find(
		&wlr_surface->addons, manager, &surface_addon_impl);
	if (!addon) {
		return false;
	}

	struct wlr_surface_invalidation_v1 *surface =
		wl_container_of(addon, surface, addon);
	struct wl_display *display =
		wl_client_get_display(wl_resource_get_client(surface->resource));

	struct wlr_surface_invalidation_v1_configure *configure = calloc(1, sizeof(*configure));
	if (!configure) {
		wl_client_post_no_memory(wl_resource_get_client(surface->resource));
		return false;
	}

	configure->serial = wl_display_next_serial(display);
	wl_list_insert(surface->configures.prev, &configure->link);
	wp_surface_invalidation_v1_send_invalidated(surface->resource, configure->serial);
	return true;
}
