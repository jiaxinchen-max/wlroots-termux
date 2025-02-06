#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "backend/termuxdc.h"
#include <termux/display/client/client.h>

struct wlr_termuxdc_backend *termuxdc_backend_from_backend(struct wlr_backend *wlr_backend) {
    assert(wlr_backend_is_termuxdc(wlr_backend));
    return (struct wlr_termuxdc_backend *) wlr_backend;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
    struct wlr_termuxdc_backend *backend = termuxdc_backend_from_backend(wlr_backend);
    backend->started = true;
    wlr_log(WLR_INFO, "Starting Termux:GUI backend");

    wl_signal_emit_mutable(&backend->backend.events.new_input, &backend->keyboard.base);
    wl_signal_emit_mutable(&backend->backend.events.new_input, &backend->pointer.base);

    for (uint32_t i = 0; i < backend->requested_outputs; i++) {
        wlr_termuxdc_output_create(&backend->backend);
    }
    return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
    struct wlr_termuxdc_backend *backend = termuxdc_backend_from_backend(wlr_backend);
    if (!wlr_backend) {
        return;
    }

    wl_list_remove(&backend->event_loop_destroy.link);
    wl_event_source_remove(backend->input_event_source);

    struct wlr_termuxdc_output *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
        wlr_output_destroy(&output->wlr_output);
    }

    wlr_allocator_destroy(backend->allocator);
    wlr_pointer_finish(&backend->pointer);
    wlr_keyboard_finish(&backend->keyboard);
    wlr_backend_finish(wlr_backend);

    DisplayDestroy();
    pthread_join(backend->input_event_thread, NULL);
    wlr_queue_destroy(&backend->event_queue);

    // close(backend->input_event_fd);
    free(backend);
}

static uint32_t get_buffer_caps(struct wlr_backend *wlr_backend) {
    return WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_DMABUF;
}

static const struct wlr_backend_impl backend_impl = {
    .start = backend_start,
    .destroy = backend_destroy,
    .get_buffer_caps = get_buffer_caps,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
    struct wlr_termuxdc_backend *backend = wl_container_of(listener, backend, event_loop_destroy);
    backend_destroy(&backend->backend);
}

static int handle_termuxdc_event(int fd, uint32_t mask, void *data) {
    struct wlr_termuxdc_backend *backend = data;

    if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
        if (mask & WL_EVENT_ERROR) {
            wlr_log(WLR_ERROR, "Failed to read from tgui event");
            wlr_backend_destroy(&backend->backend);
        }
        return 0;
    }

    eventfd_t event_count = 0;
    if (eventfd_read(backend->input_event_fd, &event_count) < 0) {
        return 0;
    }

    struct wl_list *elm = wlr_queue_pull(&backend->event_queue, true);
    if (elm == NULL) {
        wlr_log(WLR_ERROR, "tgui event queue is empty");
        return 0;
    }
    struct wlr_termuxdc_event *event = wl_container_of(elm, event, link);

    struct wlr_termuxdc_output *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
        // if (event->e.activity == output->activity) {
        //     handle_activity_event(&event->e, output);
        // }
    }
    termuxdc_event_destroy(&event->e);
    free(event);

    return 0;
}

static void *tdc_event_thread(void *data) {
    struct wlr_termuxdc_backend *backend = data;

    InputEvent event;
    while (tdc_wait_event(backend->conn, &event) == TGUI_ERR_OK) {
        struct wlr_termuxdc_event *wlr_event = calloc(1, sizeof(*wlr_event));
        if (wlr_event) {
            memcpy(&wlr_event->e, &event, sizeof(tdc_event));

            wlr_queue_push(&backend->event_queue, &wlr_event->link);

            eventfd_write(backend->tdc_event_fd, 1);
        } else {
            wlr_log(WLR_ERROR, "tgui event loss: out of memory");
            tdc_event_destroy(&event);
        }
    }

    return 0;
}

const struct wlr_pointer_impl termuxdc_pointer_impl = {
    .name = "termuxdc-pointer",
};

const struct wlr_keyboard_impl termuxdc_keyboard_impl = {
    .name = "termuxdc-keyboard",
};

struct wlr_backend *wlr_termuxdc_backend_create(struct wl_event_loop *loop) {
    wlr_log(WLR_INFO, "Creating Termux:Display client backend");

    struct wlr_termuxdc_backend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        wlr_log(WLR_ERROR, "Failed to allocate wlr_termuxdc_backend");
        return NULL;
    }
    wlr_backend_init(&backend->backend, &backend_impl);

    backend->loop = loop;
    backend->input_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);

    DisplayClientStart();
    backend->allocator = wlr_termuxdc_allocator_create(backend);

    wlr_pointer_init(&backend->pointer, &termuxdc_pointer_impl, "termuxdc-pointer");
    wlr_keyboard_init(&backend->keyboard, &termuxdc_keyboard_impl, "termuxdc-keyboard");

    wl_list_init(&backend->outputs);

    backend->event_loop_destroy.notify = handle_event_loop_destroy;
    wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

    uint32_t events = WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;
    backend->input_event_source = wl_event_loop_add_fd(backend->loop, backend->input_event_fd,
                                                      events, handle_termuxdc_event, backend);

    wlr_queue_init(&backend->event_queue);
    pthread_create(&backend->input_event_thread, NULL, tdc_event_thread, backend);

    return &backend->backend;
}

struct wlr_allocator *wlr_termuxdc_backend_get_allocator(struct wlr_termuxdc_backend *backend) {
    return backend->allocator;
}

bool wlr_backend_is_termuxdc(struct wlr_backend *backend) { return backend->impl == &backend_impl; }
