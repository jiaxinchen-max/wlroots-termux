#include <assert.h>
#include <drm_fourcc.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <termux/display/client/client.h>
#include <termux/display/client/termuxdc_event.h>

#include "backend/termuxdc.h"
#include "util/time.h"
#include "wlr/render/swapchain.h"
#include "wlr/backend/termuxdc.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
    WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_ENABLED |
    WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;

static size_t last_output_num = 0;

static struct wlr_termuxdc_output *termuxdc_output_from_output(struct wlr_output *wlr_output) {
    assert(wlr_output_is_tgui(wlr_output));
    return (struct wlr_termuxdc_output *) wlr_output;
}

static bool output_test(struct wlr_output *wlr_output, const struct wlr_output_state *state) {
    uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
    if (unsupported != 0) {
        wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%" PRIx32, unsupported);
        return false;
    }

    if (state->committed & WLR_OUTPUT_STATE_MODE) {
        assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
    }

    if (state->committed & WLR_OUTPUT_STATE_LAYERS) {
        for (size_t i = 0; i < state->layers_len; i++) {
            state->layers[i].accepted = true;
        }
    }

    return true;
}

static bool output_commit(struct wlr_output *wlr_output, const struct wlr_output_state *state) {
    struct wlr_termuxdc_output *output = termuxdc_output_from_output(wlr_output);

    if (!output_test(wlr_output, state)) {
        return false;
    }

    if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
        struct wlr_termuxdc_buffer *buffer = termuxdc_buffer_from_buffer(state->buffer);
        wlr_buffer_lock(&buffer->wlr_buffer);
        wlr_queue_push(&output->present_queue, &buffer->link);
    }

    return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
    struct wlr_termuxdc_output *output = termuxdc_output_from_output(wlr_output);
    output->present_thread_run = false;

    wl_list_remove(&output->link);
    wl_event_source_remove(output->present_complete_source);
    close(output->present_complete_fd);


    struct wl_list tmp_buffer, *tmp;
    wlr_queue_push(&output->present_queue, &tmp_buffer);
    pthread_join(output->present_thread, NULL);

    while ((tmp = wlr_queue_pull(&output->present_queue, true)) != NULL) {
        if (tmp == &tmp_buffer) {
            continue;
        }
        struct wlr_termuxdc_buffer *buf = wl_container_of(tmp, buf, link);
        wlr_buffer_unlock(&buf->wlr_buffer);
    }
    while ((tmp = wlr_queue_pull(&output->idle_queue, true)) != NULL) {
        if (tmp == &tmp_buffer) {
            continue;
        }
        struct wlr_termuxdc_buffer *buf = wl_container_of(tmp, buf, link);
        wlr_buffer_unlock(&buf->wlr_buffer);
    }
    display_destroy();
    wlr_queue_destroy(&output->present_queue);
    wlr_queue_destroy(&output->idle_queue);
    free(output);
}

static const struct wlr_output_impl output_impl = {
    .destroy = output_destroy,
    .commit = output_commit,
};

bool wlr_output_is_termuxdc(struct wlr_output *wlr_output) {
    return wlr_output->impl == &output_impl;
}

int handle_termuxdc_server_event(termuxdc_event *e, struct wlr_termuxdc_output *output) {
    uint64_t time_ms = get_current_time_msec();
    switch (e->type) {
    case EVENT_KEY: {
        handle_termuxdc_keyboard_event(e, output, time_ms);
        break;
    }
    case EVENT_TOUCH: {
        handle_termuxdc_touch_event(e, output, time_ms);
        break;
    }
    case EVENT_SCREEN_SIZE: {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_custom_mode(&state, e->screenSize.width,
                                         e->screenSize.height, DEFAULT_REFRESH);
        wlr_output_send_request_state(&output->wlr_output, &state);
        wlr_output_state_finish(&state);

        struct wlr_pointer_motion_absolute_event ev = {
            .pointer = &output->backend->pointer,
            .time_msec = time_ms,
            .x = output->cursor_x = 0.5f,
            .y = output->cursor_y = 0.5f,
        };
        wl_signal_emit_mutable(&output->backend->pointer.events.motion_absolute, &ev);
        wl_signal_emit_mutable(&output->backend->pointer.events.frame, &output->backend->pointer);
        break;
    }
    case EVENT_FRAME_COMPLETE: {
        bool redraw = false;

        if (wlr_queue_length(&output->idle_queue) > 0) {
            struct wl_list *elm = wlr_queue_pull(&output->idle_queue, true);
            struct wlr_termuxdc_buffer *buf = wl_container_of(elm, buf, link);
            wlr_buffer_unlock(&buf->wlr_buffer);
            redraw = true;
        } else if (wlr_queue_length(&output->present_queue) < WLR_SWAPCHAIN_CAP - 1) {
            redraw = true;
        }

        if (redraw) {
            wlr_output_send_frame(&output->wlr_output);
        }
        break;
    }
    default:
        break;
    }

    return 0;
}

static void *present_queue_thread(void *data) {
    struct wlr_termuxdc_output *output = data;
    output->present_thread_run = true;

    while (output->present_thread_run) {
        struct wl_list *elm = wlr_queue_pull(&output->present_queue, false);
        struct wlr_termuxdc_buffer *buffer = wl_container_of(elm, buffer, link);

        if (!output->present_thread_run) {
            wlr_queue_push(&output->idle_queue, &buffer->link);
            break;
        }

       usleep(1000000000 / DEFAULT_REFRESH);

        wlr_queue_push(&output->idle_queue, &buffer->link);

        eventfd_write(output->present_complete_fd, 1);
    }

    return 0;
}

static int present_complete(int fd, uint32_t mask, void *data) {
    struct wlr_termuxdc_output *output = data;

    if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
        if (mask & WL_EVENT_ERROR) {
            wlr_log(WLR_ERROR, "Failed to read from idle event");
        }
        return 0;
    }

    eventfd_t count = 0;
    if (eventfd_read(fd, &count) < 0) {
        return 0;
    }

    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    struct wlr_output_event_present present_event = {
        .output = &output->wlr_output,
        .commit_seq = output->wlr_output.commit_seq + 1,
        .presented = true,
        .when = &t,
        .flags = WLR_OUTPUT_PRESENT_ZERO_COPY,
    };
    wlr_output_send_present(&output->wlr_output, &present_event);
    return 0;
}

struct wlr_output *wlr_termuxdc_output_create(struct wlr_backend *wlr_backend) {
    struct wlr_termuxdc_backend *backend = termuxdc_backend_from_backend(wlr_backend);

    if (!backend->started) {
        ++backend->requested_outputs;
        return NULL;
    }

    struct wlr_termuxdc_output *output = calloc(1, sizeof(*output));
    if (output == NULL) {
        wlr_log(WLR_ERROR, "Failed to allocate wlr_termuxdc_output");
        return NULL;
    }
    output->backend = backend;

    // if (tdc_activity_create(backend->conn, &output->activity, TGUI_ACTIVITY_NORMAL, NULL,
    //                          true)) {
    //     wlr_log(WLR_ERROR, "Failed to create tdc_activity");
    //     free(output);
    //     return NULL;
    // }

    // display_client_init(1920,1080,4);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_render_format(&state, DRM_FORMAT_ABGR8888);
    wlr_output_state_set_transform(&state, WL_OUTPUT_TRANSFORM_FLIPPED_180);
    wlr_output_state_set_custom_mode(&state, 1920, 1080, DEFAULT_REFRESH);
    wlr_output_init(&output->wlr_output, &backend->backend, &output_impl, backend->loop,
                    &state);
    wlr_output_state_finish(&state);

    struct wlr_output *wlr_output = &output->wlr_output;
    wlr_output->adaptive_sync_status = WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
    wlr_output_lock_attach_render(wlr_output, true);

    size_t output_num = ++last_output_num;

    char description[128];
    snprintf(description, sizeof(description), "Termux:Display client output %zu", output_num);
    wlr_output_set_description(wlr_output, description);

    uint32_t events = WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;
    output->present_complete_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    output->present_complete_source = wl_event_loop_add_fd(
        backend->loop, output->present_complete_fd, events, present_complete, output);

    assert(output->present_complete_fd >= 0 && output->present_complete_source != NULL);

    wlr_queue_init(&output->present_queue);
    wlr_queue_init(&output->idle_queue);

    pthread_create(&output->present_thread, NULL, present_queue_thread, output);

    wl_signal_emit_mutable(&backend->backend.events.new_output, wlr_output);

    wl_list_insert(&backend->outputs, &output->link);
    return wlr_output;
}
