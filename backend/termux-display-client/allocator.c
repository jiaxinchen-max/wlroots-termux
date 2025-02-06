#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "backend/termux-display-client.h"
#include "render/drm_format_set.h"
#include "render/pixel_format.h"

static const struct wlr_buffer_impl buffer_impl;
static const struct wlr_allocator_interface allocator_impl;

struct wlr_tdc_buffer *tdc_buffer_from_buffer(struct wlr_buffer *wlr_buffer) {
    assert(wlr_buffer->impl == &buffer_impl);
    struct wlr_tdc_buffer *buffer = wl_container_of(wlr_buffer, buffer, wlr_buffer);
    return buffer;
}

static struct wlr_tdc_allocator *
tdc_allocator_from_allocator(struct wlr_allocator *wlr_allocator) {
    assert(wlr_allocator->impl == &allocator_impl);
    struct wlr_tdc_allocator *alloc = wl_container_of(wlr_allocator, alloc, wlr_allocator);
    return alloc;
}

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct wlr_tdc_buffer *buffer = tdc_buffer_from_buffer(wlr_buffer);
    if (buffer->data) {
        buffer->unlock();
    }

    wlr_dmabuf_attributes_finish(&buffer->dmabuf);
    free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
                              struct wlr_dmabuf_attributes *dmabuf) {
    struct wlr_tdc_buffer *buffer = tdc_buffer_from_buffer(wlr_buffer);
    memcpy(dmabuf, &buffer->dmabuf, sizeof(*dmabuf));
    return true;
}

static bool begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                  uint32_t flags,
                                  void **data,
                                  uint32_t *format,
                                  size_t *stride) {
    struct wlr_tdc_buffer *buffer = tdc_buffer_from_buffer(wlr_buffer);

    if (buffer->data == NULL) {
        buffer->lock(&buffer->data);
        if (buffer->data == NULL) {
            wlr_log(WLR_ERROR, "AHardwareBuffer_lock failed");
            return false;
        }
    }

    *data = buffer->data;
    *format = buffer->format;
    *stride = buffer->desc.stride * 4;
    return true;
}

static void end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    struct wlr_tdc_buffer *buffer = tdc_buffer_from_buffer(wlr_buffer);
    if (buffer->data) {
        buffer->unlock();
        buffer->data = NULL;
    }
}

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .get_dmabuf = buffer_get_dmabuf,
    .begin_data_ptr_access = begin_data_ptr_access,
    .end_data_ptr_access = end_data_ptr_access,
};


static struct wlr_buffer *allocator_create_buffer(struct wlr_allocator *wlr_allocator,
                                                  int width,
                                                  int height,
                                                  const struct wlr_drm_format *format) {
    struct wlr_tdc_allocator *alloc = tdc_allocator_from_allocator(wlr_allocator);

    if (!wlr_drm_format_has(format, DRM_FORMAT_MOD_INVALID) &&
        !wlr_drm_format_has(format, DRM_FORMAT_MOD_LINEAR)) {
        wlr_log(WLR_ERROR, "TGUI allocator only supports INVALID and "
                           "LINEAR modifiers");
        return NULL;
    }

    const struct wlr_pixel_format_info *info = drm_get_pixel_format_info(format->format);
    if (info == NULL) {
        wlr_log(WLR_ERROR, "Unsupported pixel format 0x%" PRIX32, format->format);
        return NULL;
    }

    struct wlr_tdc_buffer *buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }
    wlr_buffer_init(&buffer->wlr_buffer, &buffer_impl, width, height);

    DisplayClientInit(width,height,4);
    buffer->lock = &BeginDisplayDraw;
    buffer->unlock = &EndDisplayDraw;

    DisplayClientStart();

    wlr_log(WLR_DEBUG, "Created tdc_hardware_buffer %dx%d", width, height);


    buffer->dmabuf = (struct wlr_dmabuf_attributes) {
        .width = buffer->desc.stride,
        .height = buffer->desc.height,
        .n_planes = 1,
        .format = format->format,
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .offset[0] = 0,
        .stride[0] = buffer->desc.stride * 4,
        .fd[0] = fd,

    };

    buffer->format = format->format;
    buffer->conn = alloc->conn;
    return &buffer->wlr_buffer;

fail:
    free(buffer);
    return NULL;
}

static void allocator_destroy(struct wlr_allocator *wlr_allocator) { free(wlr_allocator); }

static const struct wlr_allocator_interface allocator_impl = {
    .destroy = allocator_destroy,
    .create_buffer = allocator_create_buffer,
};

struct wlr_allocator *wlr_tdc_allocator_create(struct wlr_tdc_backend *backend) {
    struct wlr_tdc_allocator *allocator = calloc(1, sizeof(*allocator));
    if (allocator == NULL) {
        return NULL;
    }
    allocator->conn = backend->conn;

    wlr_allocator_init(&allocator->wlr_allocator, &allocator_impl,
                       WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_DATA_PTR);

    return &allocator->wlr_allocator;
}
