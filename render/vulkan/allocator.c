#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <vulkan/vulkan.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/interface.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/render/vulkan.h>
#include <wlr/backend/interface.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <xf86drm.h>

#include "render/dmabuf.h"
#include "render/pixel_format.h"
#include "render/vulkan.h"
#include "types/wlr_buffer.h"

static void vk_allocator_ref(struct wlr_vk_allocator *vk_alloc);
static void vk_allocator_unref(struct wlr_vk_allocator *vk_alloc);

const struct wlr_buffer_impl buffer_impl;

struct wlr_vk_buffer *vulkan_buffer_from_wlr_buffer(struct wlr_buffer *wlr_buf) {
	if (!wlr_buf || wlr_buf->impl != &buffer_impl) {
		return NULL;
	}
	struct wlr_vk_buffer *buf = (struct wlr_vk_buffer *)wlr_buf;
	return buf;
}

static struct wlr_vk_buffer *vulkan_buffer_from_buffer(
		struct wlr_buffer *wlr_buf) {
	assert(wlr_buf->impl == &buffer_impl);
	return (struct wlr_vk_buffer *)wlr_buf;
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buf,
		struct wlr_dmabuf_attributes *out) {
	struct wlr_vk_buffer *buf = vulkan_buffer_from_buffer(wlr_buf);
	memcpy(out, &buf->dmabuf, sizeof(buf->dmabuf));
	return true;
}

static void buffer_destroy(struct wlr_buffer *wlr_buf) {
	struct wlr_vk_buffer *buf = vulkan_buffer_from_buffer(wlr_buf);
	wlr_dmabuf_attributes_finish(&buf->dmabuf);
	vkFreeMemory(buf->alloc->dev->dev, buf->memory, NULL);
	vkDestroyImage(buf->alloc->dev->dev, buf->image, NULL);
	vk_allocator_unref(buf->alloc);
	free(buf);
}

const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static const struct wlr_allocator_interface allocator_impl;

static struct wlr_buffer *vk_allocator_create_buffer(struct wlr_allocator *wlr_alloc,
		int width, int height, const struct wlr_drm_format *format) {
	assert (wlr_alloc->impl == &allocator_impl);
	struct wlr_vk_allocator *alloc = (struct wlr_vk_allocator *)wlr_alloc;
	const struct wlr_vk_format_props *format_props = vulkan_format_props_from_drm(alloc->dev, format->format);
	if (!format_props) {
		wlr_log(WLR_ERROR, "vk_allocator_create_buffer: no vulkan format "
				"matching drm format 0x%08x available", format->format);
		return NULL;
	}

	uint64_t *mods = calloc(format->len, sizeof(mods[0]));
	if (mods == NULL) {
		return NULL;
	}

	size_t mods_len = 0;
	for (size_t i = 0; i < format->len; i++) {
		const struct wlr_vk_format_modifier_props *mod_props =
			vulkan_format_props_find_modifier(format_props, format->modifiers[i], true);
		if (mod_props == NULL) {
			continue;
		}

		if (mod_props->max_extent.width < (uint32_t)width ||
				mod_props->max_extent.height < (uint32_t)height) {
			continue;
		}
		// TODO: add support for DISJOINT images?
		// NOTE: no idea what that means
		mods[mods_len++] = mod_props->props.drmFormatModifier;
	}

	if (mods_len == 0) {
		wlr_log(WLR_ERROR, "Found zero compatible format modifiers");
		free(mods);
		return NULL;
	}

	struct wlr_vk_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		free(mods);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &buffer_impl, width, height);
	buf->alloc = alloc;

	VkImageDrmFormatModifierListCreateInfoEXT drm_format_mod = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.drmFormatModifierCount = mods_len,
		.pDrmFormatModifiers = mods,
	};
	VkExternalMemoryImageCreateInfo ext_mem = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &drm_format_mod,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo img_create = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext_mem,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = { .width = width, .height = height, .depth = 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.format = format_props->format.vk,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.samples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkResult res = vkCreateImage(alloc->dev->dev, &img_create, NULL, &buf->image);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkCreateImage failed");
		goto error_buf;
	}

	free(mods);

	VkMemoryRequirements mem_reqs = {0};
	vkGetImageMemoryRequirements(alloc->dev->dev, buf->image, &mem_reqs);

	int mem_type_index = vulkan_find_mem_type(alloc->dev,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem_reqs.memoryTypeBits);
	if (mem_type_index == -1) {
		wlr_log(WLR_ERROR, "failed to find suitable vulkan memory type");
		goto error_buf;
	}

	VkExportMemoryAllocateInfo export_mem = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo mem_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &export_mem,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = mem_type_index,
	};

	res = vkAllocateMemory(alloc->dev->dev, &mem_alloc, NULL, &buf->memory);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkAllocateMemory failed");
		goto error_image;
	}

	res = vkBindImageMemory(alloc->dev->dev, buf->image, buf->memory, 0);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkBindImageMemory failed");
		goto error_memory;
	}

	VkImageDrmFormatModifierPropertiesEXT img_mod_props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
	};
	res = alloc->dev->api.vkGetImageDrmFormatModifierPropertiesEXT(alloc->dev->dev, buf->image, &img_mod_props);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkGetImageDrmFormatModifierPropertiesEXT failed");
		goto error_memory;
	}

	VkMemoryGetFdInfoKHR mem_get_fd = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = buf->memory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	int fd = -1;
	res = alloc->dev->api.vkGetMemoryFdKHR(alloc->dev->dev, &mem_get_fd, &fd);
	if (res != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "vkGetMemoryFdKHR failed");
		goto error_memory;
	}

	const struct wlr_vk_format_modifier_props *mod_props =
		vulkan_format_props_find_modifier(format_props, img_mod_props.drmFormatModifier, true);
	assert(mod_props != NULL);
	assert(mod_props->props.drmFormatModifierPlaneCount <= WLR_DMABUF_MAX_PLANES);

	buf->dmabuf = (struct wlr_dmabuf_attributes){
		.format = format->format,
		.modifier = img_mod_props.drmFormatModifier,
		.width = width,
		.height = height,
		.n_planes = mod_props->props.drmFormatModifierPlaneCount,
	};

	// Duplicate the first FD to all other planes
	buf->dmabuf.fd[0] = fd;
	for (uint32_t i = 1; i < mod_props->props.drmFormatModifierPlaneCount; i++) {
		int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
		if (dup_fd < 0) {
			wlr_log_errno(WLR_ERROR, "fcntl(F_DUPFD_CLOEXEC) failed");
			goto error_memory;
		}
		buf->dmabuf.fd[i] = dup_fd;
	}

	const VkImageAspectFlagBits plane_aspects[] = {
		VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
	};
	assert(mod_props->props.drmFormatModifierPlaneCount <=
		sizeof(plane_aspects) / sizeof(plane_aspects[0]));

	for (uint32_t i = 0; i < mod_props->props.drmFormatModifierPlaneCount; i++) {
		VkImageSubresource img_subres = {
			.aspectMask = plane_aspects[i],
		};
		VkSubresourceLayout subres_layout = {0};
		vkGetImageSubresourceLayout(alloc->dev->dev, buf->image, &img_subres, &subres_layout);

		buf->dmabuf.offset[i] = subres_layout.offset;
		buf->dmabuf.stride[i] = subres_layout.rowPitch;
	}

	char *format_name = drmGetFormatName(buf->dmabuf.format);
	char *modifier_name = drmGetFormatModifierName(buf->dmabuf.modifier);
	wlr_log(WLR_DEBUG, "Allocated %dx%d Vulkan buffer "
		"with format %s (0x%08"PRIX32"), modifier %s (0x%016"PRIX64")",
		buf->base.width, buf->base.height,
		format_name ? format_name : "<unknown>", buf->dmabuf.format,
		modifier_name ? modifier_name : "<unknown>", buf->dmabuf.modifier);
	free(modifier_name);
	free(format_name);

	vk_allocator_ref(alloc);

	return &buf->base;

error_memory:
	vkFreeMemory(buf->alloc->dev->dev, buf->memory, NULL);
error_image:
	vkDestroyImage(buf->alloc->dev->dev, buf->image, NULL);
error_buf:
	free(buf);
	return NULL;
}

static void vk_allocator_unref(struct wlr_vk_allocator *alloc) {
	alloc->ref_cnt--;
	if (alloc->ref_cnt == 0) {
		vulkan_device_unref(alloc->dev);
		free(alloc);
	}
}

static void vk_allocator_ref(struct wlr_vk_allocator *alloc) {
	alloc->ref_cnt++;
}

static void vk_allocator_destroy(struct wlr_allocator *wlr_alloc) {
	assert (wlr_alloc->impl == &allocator_impl);
	struct wlr_vk_allocator *alloc = (struct wlr_vk_allocator *)wlr_alloc;
	vk_allocator_unref(alloc);
}

static const struct wlr_allocator_interface allocator_impl = {
	.destroy = vk_allocator_destroy,
	.create_buffer = vk_allocator_create_buffer,
};

struct wlr_allocator *vulkan_get_allocator(struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);

	struct wlr_vk_allocator *alloc = calloc(1, sizeof(*alloc));
	if (!alloc) {
		return NULL;
	}
	alloc->backend = renderer->backend;
	alloc->dev = renderer->dev;
	vk_allocator_ref(alloc);
	vulkan_device_ref(alloc->dev);
	wlr_allocator_init(&alloc->wlr_allocator, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
	wlr_log(WLR_INFO, "Created Vulkan allocator");
	return &alloc->wlr_allocator;
}

