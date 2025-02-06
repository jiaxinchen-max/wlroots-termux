#ifndef BACKEND_TERMUX_DISPLAY_CLIENT_H
#define BACKEND_TERMUX_DISPLAY_CLIENT_H

#include <android/hardware_buffer.h>
#include <assert.h>
#include <pthread.h>
#include <termuxgui/termuxgui.h>
#include <termux/display/client/client.h>
#include <termux/display/client/InputEvent.h>

#include <wlr/backend/interface.h>
#include <wlr/backend/termuxgui.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/util/log.h>

#define DEFAULT_REFRESH (60 * 1000) // 60 Hz


struct wlr_queue {
    struct wl_list base;
    int length;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
};

static inline void wlr_queue_init(struct wlr_queue *queue) {
    queue->length = 0;
    wl_list_init(&queue->base);
    pthread_cond_init(&queue->cond, NULL);
    pthread_mutex_init(&queue->mutex, NULL);
}

static inline void wlr_queue_destroy(struct wlr_queue *queue) {
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
}

static inline struct wl_list *wlr_queue_pull(struct wlr_queue *queue, bool nonblock) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->length == 0) {
        if (nonblock) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    assert(queue->length > 0);

    queue->length--;
    struct wl_list *elm = queue->base.prev;
    wl_list_remove(elm);

    pthread_mutex_unlock(&queue->mutex);
    return elm;
}

static inline void wlr_queue_push(struct wlr_queue *queue, struct wl_list *elm) {
    pthread_mutex_lock(&queue->mutex);
    if (wl_list_empty(&queue->base)) {
        pthread_cond_signal(&queue->cond);
    }
    queue->length++;
    wl_list_insert(&queue->base, elm);
    pthread_mutex_unlock(&queue->mutex);
}

static inline int wlr_queue_length(struct wlr_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    int ret = queue->length;
    pthread_mutex_unlock(&queue->mutex);
    return ret;
}

struct wlr_tdc_backend {
    struct wlr_backend backend;
    struct wl_event_loop *loop;
    struct wlr_allocator *allocator;

    struct wlr_pointer pointer;
    struct wlr_keyboard keyboard;

    size_t requested_outputs;
    struct wl_list outputs;
    struct wl_listener event_loop_destroy;
    bool started;

    
    struct wlr_queue event_queue;
    int InputEvent_fd;
    pthread_t InputEvent_thread;
    struct wl_event_source *InputEvent_source;
};

struct wlr_tdc_allocator {
    struct wlr_allocator wlr_allocator;
   
};

struct wlr_tdc_buffer {
    struct wlr_buffer wlr_buffer;

    void *data;
    uint32_t format;
    struct wl_list link;
    struct wlr_dmabuf_attributes dmabuf;

    int (*lock)(void **outVirtualAddress);
    int (*unlock)();
};

struct wlr_tdc_output {
    struct wlr_output wlr_output;

    struct wlr_tdc_backend *backend;
    struct wl_list link;

    bool foreground;

    struct wlr_queue present_queue;
    struct wlr_queue idle_queue;
    bool present_thread_run;
    pthread_t present_thread;
    int present_complete_fd;
    struct wl_event_source *present_complete_source;

    struct {
        int id, max;
        double x, y;
        bool moved, down;
        uint64_t time_ms;
    } touch_pointer;

    double cursor_x, cursor_y;
};

struct wlr_InputEvent {
    InputEvent e;
    struct wl_list link;
};

struct wlr_tdc_backend *tdc_backend_from_backend(struct wlr_backend *wlr_backend);

struct wlr_allocator *wlr_tdc_allocator_create(struct wlr_tdc_backend *backend);

struct wlr_allocator *wlr_tdc_backend_get_allocator(struct wlr_tdc_backend *backend);

struct wlr_tdc_buffer *tdc_buffer_from_buffer(struct wlr_buffer *wlr_buffer);

int handle_activity_event(InputEvent *e, struct wlr_tdc_output *output);

void handle_touch_event(InputEvent *e, struct wlr_tdc_output *output, uint64_t time_ms);

void handle_keyboard_event(InputEvent *e, struct wlr_tdc_output *output, uint64_t time_ms);

#endif
