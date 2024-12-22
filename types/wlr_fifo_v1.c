#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fifo_v1.h>
#include <wlr/util/log.h>
#include "fifo-v1-protocol.h"

#define FIFO_MANAGER_VERSION 1

struct fifo_commit {
	struct wl_list link; // wlr_fifo_v1.fifo_commits
	bool set_barrier;
	uint32_t seq;
};

static void surface_synced_move_state(void *_dst, void *_src) {
	struct wlr_fifo_v1_surface_state *dst = _dst, *src = _src;
	dst->set_barrier = src->set_barrier;
	dst->wait_barrier = src->wait_barrier;
	src->wait_barrier = NULL;
	src->set_barrier = NULL;
}

static const struct wlr_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct wlr_fifo_v1_surface_state),
	.move_state = surface_synced_move_state,
};

static bool commit_on_valid_buffer(const struct wlr_surface * const surface) {
	if (!surface->buffer || (surface->pending.committed & WLR_SURFACE_STATE_BUFFER &&
			surface->pending.buffer == NULL)) {
		return false;
	}

	return true;
}

static void fifo_signal_barrier(struct wlr_fifo_v1 *fifo) {
	struct fifo_commit *commit, *tmp;
	wl_list_for_each_safe(commit, tmp, &fifo->commits, link) {
		wl_list_remove(&commit->link);

		wlr_surface_unlock_cached(fifo->surface, commit->seq);

		if (commit->set_barrier) {
			free(commit);
			break;
		}
		free(commit);
	}

	if (wl_list_empty(&fifo->commits)) {
		fifo->barrier_init = false;
	}
}

static void fifo_deinit(struct wlr_fifo_v1 *fifo) {
	struct fifo_commit *commit, *tmp_co;
	wl_list_for_each_safe(commit, tmp_co, &fifo->commits, link) {
		if (commit->seq) {
			wlr_surface_unlock_cached(fifo->surface, commit->seq);
		}
		wl_list_remove(&commit->link);
		free(commit);
	}
	if (fifo->output) {
		fifo->output_commit.notify = NULL;
		wl_list_remove(&fifo->output_commit.link);
		fifo->output_destroy.notify = NULL;
		wl_list_remove(&fifo->output_destroy.link);
	}
	fifo->barrier_init = false;
	fifo->current = fifo->pending = (struct wlr_fifo_v1_surface_state){0};
}

static void fifo_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, output_commit);
	fifo_deinit(fifo);
	fifo->output = NULL;
}

static void fifo_handle_output_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, output_commit);
	if (!fifo->output || !fifo->surface->buffer) {
	    return;
	}

	if (fifo->current.set_barrier) {
		fifo_signal_barrier(fifo);
	}
}

static void fifo_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_commit);
	if (fifo->current.set_barrier) {
		fifo->barrier_init = true;
	}
}

static bool fifo_do_queue_commit(struct wlr_fifo_v1 *fifo) {
	struct wlr_surface *surface = fifo->surface;
	struct wlr_surface_state *cached;
	bool pending_set_barrier = false;
	wl_list_for_each(cached, &surface->cached, cached_state_link) {
		struct wlr_fifo_v1_surface_state *state =
			wlr_surface_synced_get_state(&fifo->synced, cached);
		if (state->set_barrier) {
			pending_set_barrier = true;
			break;
		}
	}

	return (!wl_list_empty(&fifo->commits) ||
			(fifo->pending.wait_barrier &&
			 (fifo->barrier_init ||
			  (!fifo->barrier_init && pending_set_barrier))));

}
static void fifo_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_client_commit);

	if (!fifo->output) {
		wl_signal_emit_mutable(&fifo->fifo_manager->events.new_fifo,
			&(struct wlr_fifo_manager_v1_new_fifo_event){.fifo = fifo});
		return;
	}

	if (!commit_on_valid_buffer(fifo->surface)) {
		return;
	}

	if (fifo_do_queue_commit(fifo)) {
		struct fifo_commit *commit = calloc(1, sizeof(*commit));
		if (!commit) {
			wl_client_post_no_memory(wl_resource_get_client(fifo->resource));
			return;
		}

		if (fifo->pending.set_barrier) {
			commit->set_barrier = true;
		}
		commit->seq = wlr_surface_lock_pending(fifo->surface);
		wl_list_insert(fifo->commits.prev, &commit->link);
	}
}

static const struct wp_fifo_v1_interface fifo_implementation;
static struct wlr_fifo_v1 *wlr_fifo_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_v1_interface,
		&fifo_implementation));
	return wl_resource_get_user_data(resource);
}

static void fifo_handle_wait_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo =
		wlr_fifo_v1_from_resource(resource);
	fifo->pending.wait_barrier = true;
}

static void fifo_handle_set_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo =
		wlr_fifo_v1_from_resource(resource);
	fifo->pending.set_barrier = true;
}

static void fifo_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo = wlr_fifo_v1_from_resource(resource);
	fifo_deinit(fifo);
	wlr_addon_finish(&fifo->addon);
	wlr_surface_synced_finish(&fifo->synced);
	wl_list_remove(&fifo->surface_client_commit.link);
	wl_list_remove(&fifo->surface_commit.link);
	wl_signal_emit_mutable(&fifo->events.destroy, fifo);
	free(fifo);
}

static void fifo_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void surface_fifo_addon_handle_destroy(struct wlr_addon *addon) {
	struct wlr_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);
	wl_resource_destroy(fifo->resource);
}

static const struct wlr_addon_interface surface_fifo_addon_impl = {
	.name = "wp_fifo_v1",
	.destroy = surface_fifo_addon_handle_destroy,
};

static const struct wp_fifo_v1_interface fifo_implementation = {
	.destroy = fifo_handle_destroy,
	.set_barrier = fifo_handle_set_barrier,
	.wait_barrier = fifo_handle_wait_barrier
};

static struct wlr_fifo_v1 *fifo_create(struct wl_client *client, uint32_t version, uint32_t id,
		struct wlr_surface *surface) {
	struct wlr_fifo_v1 *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		goto err_alloc;
	}
	fifo->surface = surface;
	wl_list_init(&fifo->commits);

	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, version, id);
	if (fifo->resource == NULL) {
		goto err_alloc;
	}
	wl_resource_set_implementation(fifo->resource, &fifo_implementation, fifo,
		fifo_handle_resource_destroy);

	fifo->surface_client_commit.notify = fifo_handle_client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->surface_client_commit);
	fifo->surface_commit.notify = fifo_handle_commit;
	wl_signal_add(&surface->events.commit, &fifo->surface_commit);

	wl_signal_init(&fifo->events.destroy);

	wlr_log(WLR_DEBUG, "New wlr_fifo_v1 %p (res %p)", fifo, fifo->resource);

	return fifo;

err_alloc:
	free(fifo);
	return NULL;
}

static const struct wp_fifo_manager_v1_interface fifo_manager_impl;
static struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_manager_v1_interface,
		&fifo_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void fifo_manager_handle_get_fifo(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &surface_fifo_addon_impl) != NULL) {
		wl_resource_post_error(resource,
			WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"A wp_fifo_v1 object already exists for this surface");
		return;
	}

	struct wlr_fifo_v1 *fifo =
		fifo_create(wl_client, wl_resource_get_version(resource), id, surface);
	if (!fifo) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wlr_addon_init(&fifo->addon, &surface->addons, NULL, &surface_fifo_addon_impl);

	if (!wlr_surface_synced_init(&fifo->synced, surface,
			&surface_synced_impl, &fifo->pending, &fifo->current)) {
		free(fifo);
		wl_client_post_no_memory(wl_client);
		return;
	}

	struct wlr_fifo_manager_v1 *fifo_manager =
		wlr_fifo_manager_v1_from_resource(resource);

	fifo->fifo_manager = fifo_manager;
}

static void fifo_manager_handle_destroy(struct wl_client *wl_client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface fifo_manager_impl = {
	.get_fifo = fifo_manager_handle_get_fifo,
	.destroy = fifo_manager_handle_destroy,
};

static void fifo_manager_bind(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id) {
	struct wlr_fifo_manager_v1 *fifo_manager = data;
	struct wl_resource *resource =
		wl_resource_create(wl_client, &wp_fifo_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &fifo_manager_impl, fifo_manager, NULL);
}

static void fifo_manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_fifo_manager_v1 *fifo_manager =
		wl_container_of(listener, fifo_manager, display_destroy);
	wl_signal_emit_mutable(&fifo_manager->events.destroy, fifo_manager);
	wl_list_remove(&fifo_manager->display_destroy.link);
	wl_global_destroy(fifo_manager->global);
	free(fifo_manager);
}

struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(struct wl_display *display, uint32_t version) {
	assert(version <= FIFO_MANAGER_VERSION);

	struct wlr_fifo_manager_v1 *fifo_manager = calloc(1, sizeof(*fifo_manager));
	if (!fifo_manager) {
		return NULL;
	}

	fifo_manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface,
		version, fifo_manager, fifo_manager_bind);
	if (!fifo_manager->global) {
		free(fifo_manager);
		return NULL;
	}

	wl_signal_init(&fifo_manager->events.destroy);
	wl_signal_init(&fifo_manager->events.new_fifo);

	fifo_manager->display_destroy.notify = fifo_manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &fifo_manager->display_destroy);

	return fifo_manager;
}

void wlr_fifo_v1_set_output(struct wlr_fifo_v1 *fifo, struct wlr_output *output) {
	// reset fifo state
	fifo_deinit(fifo);

	// handle new output
	fifo->output = output;
	fifo->output_commit.notify = fifo_handle_output_commit;
	wl_signal_add(&fifo->output->events.commit, &fifo->output_commit);
	fifo->output_destroy.notify = fifo_handle_output_destroy;
	wl_signal_add(&fifo->output->events.destroy, &fifo->output_destroy);
}
