#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_commit_timing_v1.h>
#include <wlr/util/log.h>
#include "commit-timing-v1-protocol.h"
#include "util/time.h"

#define TIMING_MANAGER_VERSION 1

static int handle_commit_timer(void *data) {
	struct wlr_commit_timer_v1_commit *commit = data;
	// the remove from the list must happen before unlocking the commit, since the commit might end
	// up calling wlr_commit_timer_v1_set_output(), which traverses this list.
	wl_list_remove(&commit->link);
	wlr_surface_unlock_cached(commit->timer->surface, commit->pending_seq);
	free(commit);
	return 0;
}

static bool is_timestamp_valid(const struct wlr_commit_timer_v1 *timer, uint64_t time_nsec) {
	// no conditions trigger invalid timestamp right now
	return true;
}

static void timer_handle_output_present(struct wl_listener *listener, void *data) {
	struct wlr_commit_timer_v1 *timer =
		wl_container_of(listener, timer, present);
	struct wlr_output_event_present *event = data;

	// we need to have just one presentation time so that, together with the refresh rate, we
	// can know the refresh cycle offset for future presentations.
	if (event->presented) {
		timer->output.base_present_nsec = timespec_to_nsec(&event->when);
		wl_list_remove(&timer->present.link);
		timer->present.notify = NULL;
	}
}

static uint64_t timer_get_target_present_nsec(struct wlr_commit_timer_v1_commit
		*commit) {
	uint64_t target_nsec = 0;
	struct wlr_output *output = commit->timer->output.output;
	if (!output || !output->refresh) {
		goto out;
	}

	struct wlr_commit_timer_v1 *timer = commit->timer;
	uint64_t refresh_nsec = output->refresh;
	uint64_t cycle_phase_nsec = timer->output.base_present_nsec % refresh_nsec;

	uint64_t round_to_nearest_refresh_nsec = commit->timestamp_nsec;
	round_to_nearest_refresh_nsec -= cycle_phase_nsec;
	round_to_nearest_refresh_nsec += refresh_nsec/2;
	round_to_nearest_refresh_nsec -= (round_to_nearest_refresh_nsec % refresh_nsec);
	round_to_nearest_refresh_nsec += cycle_phase_nsec;

	target_nsec = round_to_nearest_refresh_nsec;
out:
	return target_nsec;
}

static int mhz_to_nsec(int mhz) {
	assert(mhz != 0);
	return 1000000000000LL / mhz;
}

static void timer_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_commit_timer_v1 *timer =
		wl_container_of(listener, timer, client_commit);

	if (!timer->output.output) {
		wl_signal_emit_mutable(&timer->timing_manager->events.new_timer,
			&(struct wlr_commit_timing_manager_v1_new_timer_event){.timer = timer});
		goto out;
	}

	// we don't have a .set_timestamp request for this commit
	if (!timer->timestamp_nsec) {
		goto out;
	}

	// we have a .set_timestamp request for this commit, but it is invalid
	if (!is_timestamp_valid(timer, timer->timestamp_nsec)) {
		goto out;
	}

	struct wlr_output *output = timer->output.output;
	// Either the output doesn't have a notion of refresh rate or we haven't received
	// our first presentation yet.
	if (!output->refresh || !timer->output.base_present_nsec) {
		goto out;
	}
	// if the refresh rate for the output was changed, reset
	if (timer->output.refresh != output->refresh) {
		wlr_commit_timer_v1_set_output(timer, output);
		goto out;
	}

	struct wlr_commit_timer_v1_commit *commit = calloc(1, sizeof(*commit));
	if (!commit) {
		goto err_alloc;
	}
	commit->timer = timer;
	commit->timestamp_nsec = timer->timestamp_nsec;
	timer->timestamp_nsec = 0;

	uint64_t target_nsec = timer_get_target_present_nsec(commit);
	if (!target_nsec) {
		goto err_target_nsec;
	}

	// Calculate the time until the beginning of the refresh cycle before the one we are targeting,
	// subtracting a 1ms slop. This guarantees that the surface commit is unlocked before the
	// compositor receives the .frame event for the refresh cycle we want to target.
	int64_t delay_target_msec = target_nsec; // time of target presentation
	delay_target_msec -= mhz_to_nsec(timer->output.refresh); // subtract 1 refresh cycle
	delay_target_msec -= 1000000; // give a 1msec slop to compensate for possible mismatch
	delay_target_msec /= 1000000; // to msec
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t now_msec = timespec_to_msec(&now);
	delay_target_msec -= now_msec; // delay time
	// If we are too close to the target time, don't bother and just commit.
	// This number is just a heuristic.
	if (delay_target_msec < 1) {
		goto out;
	}

	commit->unlock_timer =
		wl_event_loop_add_timer(wl_display_get_event_loop(timer->wl_display),
			handle_commit_timer, commit);
	if (!commit->unlock_timer) {
		goto err_alloc;
	}
	wl_event_source_timer_update(commit->unlock_timer, delay_target_msec);

	commit->pending_seq = wlr_surface_lock_pending(timer->surface);

	wl_list_insert(&timer->commits, &commit->link);

	return;

err_alloc:
	wl_client_post_no_memory(wl_resource_get_client(timer->resource));
err_target_nsec:
	free(commit);
out:
	timer->timestamp_nsec = 0;
}

static const struct wp_commit_timer_v1_interface timer_impl;

static struct wlr_commit_timer_v1 *wlr_commit_timer_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timer_v1_interface,
		&timer_impl));
	return wl_resource_get_user_data(resource);
}

static void timer_handle_set_timestamp(struct wl_client *client,
		struct wl_resource *resource, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
		uint32_t tv_nsec) {
	struct wlr_commit_timer_v1 *timer = wlr_commit_timer_v1_from_resource(resource);

	if (timer->timestamp_nsec) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
			"surface already has a timestamp");
		return;
	}

	uint64_t timestamp_nsec =
		timespec_to_nsec(&(struct timespec){ .tv_sec = (uint64_t)tv_sec_hi<<32 | tv_sec_lo,
			.tv_nsec = tv_nsec });
	if (!is_timestamp_valid(timer, timestamp_nsec)) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
			"client provided an invalid timestamp");
		return;
	}

	timer->timestamp_nsec = timestamp_nsec;
}

static void timer_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_commit_timer_v1_interface timer_impl = {
	.destroy = timer_handle_destroy,
	.set_timestamp = timer_handle_set_timestamp
};

static void surface_addon_destroy(struct wlr_addon *addon) {
    struct wlr_commit_timer_v1 *timer = wl_container_of(addon, timer, addon);
    wl_resource_destroy(timer->resource);
}

static const struct wlr_addon_interface surface_addon_impl = {
    .name = "wp_commit_timer_v1",
    .destroy = surface_addon_destroy,
};

static void timer_deinit(struct wlr_commit_timer_v1 *timer) {
	struct wlr_commit_timer_v1_commit *commit, *tmp_co;
	wl_list_for_each_safe(commit, tmp_co, &timer->commits, link) {
		if (commit->unlock_timer) {
			wl_event_source_remove(commit->unlock_timer);
		}
		wlr_surface_unlock_cached(commit->timer->surface, commit->pending_seq);
		wl_list_remove(&commit->link);
		free(commit);
	}
	if (timer->present.notify) {
		timer->present.notify = NULL;
		wl_list_remove(&timer->present.link);
	}
}

static void timer_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_commit_timer_v1 *timer = wlr_commit_timer_v1_from_resource(resource);
	timer_deinit(timer);
	wlr_addon_finish(&timer->addon);
	wl_list_remove(&timer->client_commit.link);
	wl_signal_emit_mutable(&timer->timing_manager->events.timer_destroy, timer);
	free(timer);
}

static struct wlr_commit_timer_v1 *commit_timer_create(struct wl_client *wl_client, uint32_t version,
		uint32_t id, struct wlr_surface *surface) {
	struct wlr_commit_timer_v1 *timer = calloc(1, sizeof(*timer));
	if (timer == NULL) {
		goto err_alloc;
	}

	timer->resource = wl_resource_create(wl_client, &wp_commit_timer_v1_interface, version, id);
	if (timer->resource == NULL) {
		goto err_alloc;
	}
	wl_resource_set_implementation(timer->resource, &timer_impl, timer,
		timer_handle_resource_destroy);

	wl_list_init(&timer->commits);

	/* we will use the wl_display to add a timer to the wl_event_loop */
	timer->wl_display = wl_client_get_display(wl_client);

	timer->surface = surface;
	timer->client_commit.notify = timer_handle_client_commit;
	wl_signal_add(&timer->surface->events.client_commit, &timer->client_commit);

	wlr_log(WLR_DEBUG, "New wlr_commit_timer_v1 %p (res %p)", timer, timer->resource);

	return timer;

err_alloc:
	free(timer);
	return NULL;
}

static const struct wp_commit_timing_manager_v1_interface timing_manager_impl;
static struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timing_manager_v1_interface,
		&timing_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void timing_manager_handle_get_timer(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &surface_addon_impl) != NULL) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
			"A wp_commit_timer_v1 object already exists for this surface");
		return;
	}

	struct wlr_commit_timer_v1 *timer =
		commit_timer_create(wl_client, wl_resource_get_version(resource), id, surface);
	if (!timer) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wlr_addon_init(&timer->addon, &surface->addons, NULL, &surface_addon_impl);

	struct wlr_commit_timing_manager_v1 *manager =
		wlr_commit_timing_manager_v1_from_resource(resource);

	timer->timing_manager = manager;
}

static void timing_manager_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_commit_timing_manager_v1_interface timing_manager_impl = {
	.get_timer = timing_manager_handle_get_timer,
	.destroy = timing_manager_handle_destroy,
};

static void timing_manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wp_commit_timing_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct wlr_commit_timing_manager_v1 *manager = data;
	wl_resource_set_implementation(resource, &timing_manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_commit_timing_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, manager);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_create(struct wl_display *display,
		uint32_t version) {
	assert(version <= TIMING_MANAGER_VERSION);

	struct wlr_commit_timing_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		goto err_out;
	}

	manager->global = wl_global_create(display, &wp_commit_timing_manager_v1_interface,
		version, manager, timing_manager_bind);
	if (!manager->global) {
		goto err_out;
	}

	wl_signal_init(&manager->events.new_timer);
	wl_signal_init(&manager->events.timer_destroy);
	wl_signal_init(&manager->events.destroy);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;

err_out:
	free(manager);
	return NULL;
}

void wlr_commit_timer_v1_set_output(struct wlr_commit_timer_v1 *timer,
		struct wlr_output *output) {
	timer->output.output = output;
	// We need to have just one presentation time so that, together with the refresh rate, we
	// can know the refresh cycle offset for future presentations.
	timer->output.base_present_nsec = 0;

	timer_deinit(timer);
	// we make a copy of the refresh rate so that we can check for whenever it changes
	timer->output.refresh = output->refresh;
	timer->present.notify = timer_handle_output_present;
	wl_signal_add(&output->events.present, &timer->present);
}
