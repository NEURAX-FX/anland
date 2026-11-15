#include "vulkan_output.h"
#include "protocol.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AnlandVulkan", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AnlandVulkan", __VA_ARGS__)
#define VK_OK(expr) vk_ok((expr), #expr)

enum { FRAMES_IN_FLIGHT = 2, TIMING_CACHE_SIZE = 64 };
enum { ANLAND_RGBA_8888 = 1, RGBA_BYTES = 4 };
static const uint64_t wsi_timeout_ns = 1000000000ULL;
static const VkExternalMemoryHandleTypeFlagBits dmabuf = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
static const VkExternalSemaphoreHandleTypeFlagBits sync_fd = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
static const VkImageSubresourceRange color_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
static const uint32_t vert_spirv[] = {
#include "vulkan_output_vert.inc"
};
static const uint32_t frag_spirv[] = {
#include "vulkan_output_frag.inc"
};

/* Recycled only after this draw's submission fence signals. */
struct draw_resources {
    VkSemaphore image_available;
    VkSemaphore producer_done;
    VkSemaphore staging_released;
    VkFence submitted;
};

struct swap_image {
    VkImageView view;
    VkFramebuffer framebuffer;
    VkCommandBuffer draw;
    /* A submit fence does not cover present's semaphore wait. Reuse this
     * semaphore only after acquiring and waiting on the same WSI image. */
    VkSemaphore ready_to_present;
};

struct presentation_sample {
    uint32_t id;
    uint64_t actual_ns;
};

struct vulkan_output {
    pthread_t owner;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkGetPastPresentationTimingGOOGLE get_past_timing;

    VkImage staging;
    VkDeviceMemory staging_memory;
    VkImageView staging_view;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkRenderPass render_pass;
    VkPipeline pipeline;
    VkCommandPool command_pool;

    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    VkFormat format;
    struct swap_image *images;
    uint32_t image_count;
    struct draw_resources draws[FRAMES_IN_FLIGHT];
    uint32_t next_draw;
    uint32_t image_index;
    bool awaiting_present;
    bool failed;
    uint32_t last_present_id;
    struct presentation_sample timings[TIMING_CACHE_SIZE];
};

static bool vk_ok(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS)
        return true;
    LOGE("%s failed: VkResult %d", operation, result);
    return false;
}

static bool has_extension(const VkExtensionProperties *props, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(props[i].extensionName, name) == 0)
            return true;
    }
    return false;
}

static bool select_queue(struct vulkan_output *out, VkPhysicalDevice physical)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *props = calloc(count, sizeof(*props));
    if (!props)
        return false;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!props[i].queueCount || !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        VkBool32 present = VK_FALSE;
        if (VK_OK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, out->surface, &present)) && present) {
            out->family = i;
            found = true;
            break;
        }
    }
    free(props);
    return found;
}

static bool create_device(struct vulkan_output *out, VkPhysicalDevice physical)
{
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
    VkPhysicalDeviceProperties core_props;
    vkGetPhysicalDeviceProperties(physical, &core_props);
    if (core_props.apiVersion < VK_API_VERSION_1_2)
        return false;
    vkGetPhysicalDeviceProperties2(physical, &props);
    /* Match the vendor Vulkan driver used to validate the cross-driver handoff,
     * not software Vulkan or an optionally installed Mesa Android driver. */
    if (driver.driverID != VK_DRIVER_ID_ARM_PROPRIETARY)
        return false;

    const char *extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, /* Optional. */
    };
    const uint32_t required = sizeof(extensions) / sizeof(extensions[0]) - 1;
    uint32_t count = 0;
    if (!VK_OK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL)))
        return false;
    VkExtensionProperties *available = calloc(count, sizeof(*available));
    if (!available)
        return false;
    bool ok = VK_OK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, available));
    for (uint32_t i = 0; ok && i < required; ++i) {
        if (!has_extension(available, count, extensions[i])) {
            LOGE("%s does not support %s", props.properties.deviceName, extensions[i]);
            ok = false;
        }
    }
    const bool timing = ok && has_extension(available, count, extensions[required]);
    free(available);
    if (!ok || !select_queue(out, physical))
        return false;

    const VkPhysicalDeviceExternalSemaphoreInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .handleType = sync_fd,
    };
    VkExternalSemaphoreProperties sem_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &sem_info, &sem_props);
    const VkExternalSemaphoreFeatureFlags features = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
                                                     VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
    if ((sem_props.externalSemaphoreFeatures & features) != features) {
        LOGE("SYNC_FD semaphore import/export is unavailable");
        return false;
    }

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = out->family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = required + timing, .ppEnabledExtensionNames = extensions,
    };
    if (!VK_OK(vkCreateDevice(physical, &device, NULL, &out->device)))
        return false;
    out->physical = physical;
    vkGetDeviceQueue(out->device, out->family, 0, &out->queue);
    out->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(out->device, "vkGetMemoryFdPropertiesKHR");
    out->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(out->device, "vkImportSemaphoreFdKHR");
    out->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)
        vkGetDeviceProcAddr(out->device, "vkGetSemaphoreFdKHR");
    if (timing) {
        out->get_past_timing = (PFN_vkGetPastPresentationTimingGOOGLE)
            vkGetDeviceProcAddr(out->device, "vkGetPastPresentationTimingGOOGLE");
    }
    LOGI("selected %s (%s), actual presentation timing %s", props.properties.deviceName,
         driver.driverName, out->get_past_timing ? "available" : "unavailable");
    if (!out->get_memory_fd_properties || !out->import_semaphore_fd || !out->get_semaphore_fd) {
        LOGE("external memory or semaphore FD entry points unavailable");
        return false;
    }
    return true;
}

static bool init_vulkan(struct vulkan_output *out, ANativeWindow *window)
{
    uint32_t version = 0;
    if (!VK_OK(vkEnumerateInstanceVersion(&version)) || version < VK_API_VERSION_1_2) {
        LOGE("Vulkan 1.2 loader is required");
        return false;
    }
    const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Anland consumer",
        .apiVersion = VK_API_VERSION_1_2,
    };
    const VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions,
    };
    if (!VK_OK(vkCreateInstance(&instance, NULL, &out->instance)))
        return false;
    const VkAndroidSurfaceCreateInfoKHR surface = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = window,
    };
    if (!VK_OK(vkCreateAndroidSurfaceKHR(out->instance, &surface, NULL, &out->surface)))
        return false;

    uint32_t count = 0;
    if (!VK_OK(vkEnumeratePhysicalDevices(out->instance, &count, NULL)) || !count)
        return false;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices)
        return false;
    bool found = false;
    if (VK_OK(vkEnumeratePhysicalDevices(out->instance, &count, devices))) {
        for (uint32_t i = 0; i < count && !out->device; ++i) {
            if (create_device(out, devices[i])) {
                found = true;
                break;
            }
        }
    }
    free(devices);
    if (!found)
        LOGE("no compatible ARM vendor Vulkan device for GPU staging");
    return found;
}

static bool create_view(VkDevice device, VkImage image, VkFormat format, VkImageView *view)
{
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = color_range,
    };
    return VK_OK(vkCreateImageView(device, &info, NULL, view));
}

static bool import_staging(struct vulkan_output *out, int staging_fd, const struct buf_info *slot)
{
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = slot->modifier, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modifier, .handleType = dmabuf,
    };
    const VkPhysicalDeviceImageFormatInfo2 format_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &external,
        .format = format, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkExternalImageFormatProperties external_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 format_props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props,
    };
    if (!VK_OK(vkGetPhysicalDeviceImageFormatProperties2(out->physical, &format_info, &format_props)))
        return false;
    if (!(external_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        slot->width > format_props.imageFormatProperties.maxExtent.width ||
        slot->height > format_props.imageFormatProperties.maxExtent.height) {
        LOGE("cannot sample imported LINEAR RGBA8 DMA-BUF at %ux%u", slot->width, slot->height);
        return false;
    }

    /* Linear sampling is a property of this modifier, not of optimal tiling. */
    VkDrmFormatModifierPropertiesListEXT modifiers = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &modifiers };
    vkGetPhysicalDeviceFormatProperties2(out->physical, format, &props);
    modifiers.pDrmFormatModifierProperties = calloc(modifiers.drmFormatModifierCount,
                                                      sizeof(*modifiers.pDrmFormatModifierProperties));
    if (!modifiers.pDrmFormatModifierProperties)
        return false;
    vkGetPhysicalDeviceFormatProperties2(out->physical, format, &props);
    bool linear_filter = false;
    for (uint32_t i = 0; i < modifiers.drmFormatModifierCount; ++i) {
        const VkDrmFormatModifierPropertiesEXT *m = &modifiers.pDrmFormatModifierProperties[i];
        if (m->drmFormatModifier == slot->modifier) {
            linear_filter = m->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            break;
        }
    }
    free(modifiers.pDrmFormatModifierProperties);

    const VkSubresourceLayout plane = { .offset = slot->offset, .rowPitch = slot->stride };
    const VkImageDrmFormatModifierExplicitCreateInfoEXT layout = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = slot->modifier, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane,
    };
    const VkExternalMemoryImageCreateInfo handles = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &layout, .handleTypes = dmabuf,
    };
    const VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &handles, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {slot->width, slot->height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = format_info.tiling, .usage = format_info.usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!VK_OK(vkCreateImage(out->device, &image, NULL, &out->staging)))
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(out->device, out->staging, &req);
    const off_t size = lseek(staging_fd, 0, SEEK_END);
    if (size < 0 || (VkDeviceSize)size < req.size || (VkDeviceSize)size < (uint64_t)slot->stride * slot->height) {
        LOGE("staging DMA-BUF is smaller than the image requirement");
        return false;
    }
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (!VK_OK(out->get_memory_fd_properties(out->device, dmabuf, staging_fd, &fd_props)))
        return false;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(out->physical, &memory);
    uint32_t type = 0;
    const uint32_t memory_types = req.memoryTypeBits & fd_props.memoryTypeBits;
    for (; type < memory.memoryTypeCount; ++type) {
        if ((memory_types & (1u << type)) && !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT))
            break;
    }
    if (type == memory.memoryTypeCount) {
        LOGE("no compatible memory type for the staging DMA-BUF");
        return false;
    }
    int fd = fcntl(staging_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        LOGE("dup staging DMA-BUF: %s", strerror(errno));
        return false;
    }
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = out->staging,
    };
    const VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated, .handleType = dmabuf, .fd = fd,
    };
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = req.size, .memoryTypeIndex = type,
    };
    if (!VK_OK(vkAllocateMemory(out->device, &allocation, NULL, &out->staging_memory))) {
        close(fd);
        return false;
    }
    const VkFilter filter = linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = filter, .minFilter = filter,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    return VK_OK(vkBindImageMemory(out->device, out->staging, out->staging_memory, 0)) &&
           create_view(out->device, out->staging, format, &out->staging_view) &&
           VK_OK(vkCreateSampler(out->device, &sampler, NULL, &out->sampler));
}

static bool select_surface_format(struct vulkan_output *out, VkSurfaceFormatKHR *format)
{
    uint32_t count = 0;
    if (!VK_OK(vkGetPhysicalDeviceSurfaceFormatsKHR(out->physical, out->surface, &count, NULL)) || !count)
        return false;
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (!formats)
        return false;
    bool found = false;
    if (VK_OK(vkGetPhysicalDeviceSurfaceFormatsKHR(out->physical, out->surface, &count, formats))) {
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                continue;
            if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
                (count == 1 && formats[i].format == VK_FORMAT_UNDEFINED)) {
                *format = (VkSurfaceFormatKHR){ VK_FORMAT_R8G8B8A8_UNORM, formats[i].colorSpace };
                found = true;
                break;
            }
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
                *format = formats[i];
                found = true;
            }
        }
    }
    free(formats);
    if (!found)
        LOGE("no compatible UNORM RGBA swapchain format");
    return found;
}

static bool create_swapchain(struct vulkan_output *out, ANativeWindow *window)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR format;
    if (!VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(out->physical, out->surface, &caps)) ||
        !select_surface_format(out, &format))
        return false;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        const int width = ANativeWindow_getWidth(window), height = ANativeWindow_getHeight(window);
        if (width <= 0 || height <= 0)
            return false;
        extent = (VkExtent2D){ (uint32_t)width, (uint32_t)height };
    }
    if (!extent.width || !extent.height || extent.width < caps.minImageExtent.width ||
        extent.height < caps.minImageExtent.height || extent.width > caps.maxImageExtent.width ||
        extent.height > caps.maxImageExtent.height ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        LOGE("invalid Android swapchain extent or usage");
        return false;
    }
    const VkCompositeAlphaFlagBitsKHR alpha_modes[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = 0;
    for (size_t i = 0; i < sizeof(alpha_modes) / sizeof(alpha_modes[0]); ++i) {
        if (caps.supportedCompositeAlpha & alpha_modes[i]) {
            alpha = alpha_modes[i];
            break;
        }
    }
    if (!alpha)
        return false;

    uint32_t count = caps.minImageCount;
    if (count < FRAMES_IN_FLIGHT + 1 && (!caps.maxImageCount || count < caps.maxImageCount))
        ++count;
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = out->surface,
        .minImageCount = count, .imageFormat = format.format, .imageColorSpace = format.colorSpace,
        .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?
                        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
        .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    out->extent = extent;
    out->format = format.format;
    return VK_OK(vkCreateSwapchainKHR(out->device, &info, NULL, &out->swapchain));
}

static bool create_descriptors(struct vulkan_output *out)
{
    const VkDescriptorSetLayoutBinding binding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding,
    };
    const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    const VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size,
    };
    if (!VK_OK(vkCreateDescriptorSetLayout(out->device, &layout, NULL, &out->set_layout)) ||
        !VK_OK(vkCreateDescriptorPool(out->device, &pool, NULL, &out->descriptor_pool)))
        return false;
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = out->descriptor_pool,
        .descriptorSetCount = 1, .pSetLayouts = &out->set_layout,
    };
    if (!VK_OK(vkAllocateDescriptorSets(out->device, &allocate, &out->descriptor_set)))
        return false;
    const VkDescriptorImageInfo image = { out->sampler, out->staging_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out->descriptor_set,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(out->device, 1, &write, 0, NULL);
    const VkPipelineLayoutCreateInfo pipeline_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &out->set_layout,
    };
    return VK_OK(vkCreatePipelineLayout(out->device, &pipeline_layout, NULL, &out->pipeline_layout));
}

static bool create_render_pass(struct vulkan_output *out)
{
    const VkAttachmentDescription color = {
        .format = out->format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &reference,
    };
    /* The implicit attachment transition must happen after the WSI semaphore
     * wait in COLOR_ATTACHMENT_OUTPUT, not earlier in the pipeline. */
    const VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    const VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &color,
        .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency,
    };
    return VK_OK(vkCreateRenderPass(out->device, &info, NULL, &out->render_pass));
}

static bool create_pipeline(struct vulkan_output *out)
{
    const VkShaderModuleCreateInfo vert = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vert_spirv), .pCode = vert_spirv,
    };
    const VkShaderModuleCreateInfo frag = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(frag_spirv), .pCode = frag_spirv,
    };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool ok = VK_OK(vkCreateShaderModule(out->device, &vert, NULL, &vs)) &&
              VK_OK(vkCreateShaderModule(out->device, &frag, NULL, &fs));
    if (ok) {
        const VkPipelineShaderStageCreateInfo stages[] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
        };
        const VkPipelineVertexInputStateCreateInfo vertex = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        const VkPipelineInputAssemblyStateCreateInfo topology = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        const VkViewport viewport = { 0, 0, out->extent.width, out->extent.height, 0, 1 };
        const VkRect2D scissor = {{0, 0}, out->extent};
        const VkPipelineViewportStateCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
        };
        const VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f,
        };
        const VkPipelineMultisampleStateCreateInfo samples = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkPipelineColorBlendAttachmentState attachment = {
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &attachment,
        };
        const VkGraphicsPipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &topology,
            .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples,
            .pColorBlendState = &blend, .layout = out->pipeline_layout, .renderPass = out->render_pass,
        };
        ok = VK_OK(vkCreateGraphicsPipelines(out->device, VK_NULL_HANDLE, 1, &info, NULL, &out->pipeline));
    }
    vkDestroyShaderModule(out->device, fs, NULL);
    vkDestroyShaderModule(out->device, vs, NULL);
    return ok;
}

static void staging_owner(const struct vulkan_output *out, VkCommandBuffer cmd, bool acquire)
{
    /* The external owner hands off the contents in GENERAL, including on the
     * first frame. An UNDEFINED acquire would discard the producer's pixels. */
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0,
        .oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : out->family,
        .dstQueueFamilyIndex = acquire ? out->family : VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image = out->staging, .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(cmd, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
}

static bool record_draw(struct vulkan_output *out, const struct swap_image *image)
{
    /* Reacquiring an image does not necessarily mean its previous command
     * buffer has left pending state. WSI still orders GPU access to the image. */
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    if (!VK_OK(vkBeginCommandBuffer(image->draw, &begin)))
        return false;
    staging_owner(out, image->draw, true);
    const VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = out->render_pass,
        .framebuffer = image->framebuffer, .renderArea = {{0, 0}, out->extent},
    };
    vkCmdBeginRenderPass(image->draw, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(image->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, out->pipeline);
    vkCmdBindDescriptorSets(image->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, out->pipeline_layout,
                            0, 1, &out->descriptor_set, 0, NULL);
    vkCmdDraw(image->draw, 3, 1, 0, 0);
    vkCmdEndRenderPass(image->draw);
    staging_owner(out, image->draw, false);
    return VK_OK(vkEndCommandBuffer(image->draw));
}

static bool init_swap_images(struct vulkan_output *out)
{
    uint32_t count = 0;
    if (!VK_OK(vkGetSwapchainImagesKHR(out->device, out->swapchain, &count, NULL)) || !count)
        return false;
    VkImage *images = calloc(count, sizeof(*images));
    if (!images)
        return false;
    bool ok = VK_OK(vkGetSwapchainImagesKHR(out->device, out->swapchain, &count, images));
    if (ok) {
        out->images = calloc(count, sizeof(*out->images));
        ok = out->images != NULL;
    }
    const VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    const VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = out->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    if (ok)
        out->image_count = count;
    for (uint32_t i = 0; ok && i < count; ++i) {
        struct swap_image *image = &out->images[i];
        const VkFramebufferCreateInfo framebuffer = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = out->render_pass,
            .attachmentCount = 1, .pAttachments = &image->view,
            .width = out->extent.width, .height = out->extent.height, .layers = 1,
        };
        ok = create_view(out->device, images[i], out->format, &image->view) &&
             VK_OK(vkCreateFramebuffer(out->device, &framebuffer, NULL, &image->framebuffer)) &&
             VK_OK(vkCreateSemaphore(out->device, &semaphore, NULL, &image->ready_to_present)) &&
             VK_OK(vkAllocateCommandBuffers(out->device, &commands, &image->draw)) && record_draw(out, image);
    }
    free(images);
    return ok;
}

static bool init_draw_resources(struct vulkan_output *out)
{
    const VkCommandPoolCreateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = out->family,
    };
    if (!VK_OK(vkCreateCommandPool(out->device, &commands, NULL, &out->command_pool)))
        return false;
    const VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    const VkExportSemaphoreCreateInfo export = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = sync_fd,
    };
    const VkSemaphoreCreateInfo release = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export };
    const VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        struct draw_resources *r = &out->draws[i];
        if (!VK_OK(vkCreateSemaphore(out->device, &semaphore, NULL, &r->image_available)) ||
            !VK_OK(vkCreateSemaphore(out->device, &semaphore, NULL, &r->producer_done)) ||
            !VK_OK(vkCreateSemaphore(out->device, &release, NULL, &r->staging_released)) ||
            !VK_OK(vkCreateFence(out->device, &fence, NULL, &r->submitted)))
            return false;
    }
    return init_swap_images(out);
}

struct vulkan_output *vulkan_output_create(ANativeWindow *window, int staging_fd,
                                           const struct buf_info *info)
{
    if (!window || staging_fd < 0 || !info || !info->width || !info->height ||
        info->format != ANLAND_RGBA_8888 || info->offset || info->modifier || info->stride % RGBA_BYTES ||
        info->stride < (uint64_t)info->width * RGBA_BYTES) {
        LOGE("expected one LINEAR RGBA8 staging DMA-BUF with zero offset");
        return NULL;
    }
    struct vulkan_output *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    out->owner = pthread_self();
    if (!init_vulkan(out, window) || !import_staging(out, staging_fd, info) ||
        !create_swapchain(out, window) || !create_descriptors(out) || !create_render_pass(out) ||
        !create_pipeline(out) || !init_draw_resources(out)) {
        vulkan_output_destroy(out);
        return NULL;
    }
    LOGI("GPU staging %ux%u stride=%u -> Vulkan WSI %ux%u (%u images)", info->width, info->height,
         info->stride, out->extent.width, out->extent.height, out->image_count);
    return out;
}

void vulkan_output_destroy(struct vulkan_output *out)
{
    if (!out)
        return;
    if (!pthread_equal(out->owner, pthread_self())) {
        LOGE("Vulkan teardown must run on the render thread");
        return;
    }
    VkDevice device = out->device;
    if (device) {
        /* The only idle wait: submissions must release their references before
         * deleting resources, including the imported staging DMA-BUF. */
        VK_OK(vkDeviceWaitIdle(device));
        vkDestroyCommandPool(device, out->command_pool, NULL);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            const struct draw_resources *r = &out->draws[i];
            vkDestroyFence(device, r->submitted, NULL);
            vkDestroySemaphore(device, r->staging_released, NULL);
            vkDestroySemaphore(device, r->producer_done, NULL);
            vkDestroySemaphore(device, r->image_available, NULL);
        }
        for (uint32_t i = 0; i < out->image_count; ++i) {
            const struct swap_image *image = &out->images[i];
            vkDestroySemaphore(device, image->ready_to_present, NULL);
            vkDestroyFramebuffer(device, image->framebuffer, NULL);
            vkDestroyImageView(device, image->view, NULL);
        }
        free(out->images);
        vkDestroyPipeline(device, out->pipeline, NULL);
        vkDestroyRenderPass(device, out->render_pass, NULL);
        vkDestroyPipelineLayout(device, out->pipeline_layout, NULL);
        vkDestroyDescriptorPool(device, out->descriptor_pool, NULL);
        vkDestroyDescriptorSetLayout(device, out->set_layout, NULL);
        vkDestroySampler(device, out->sampler, NULL);
        vkDestroyImageView(device, out->staging_view, NULL);
        vkDestroyImage(device, out->staging, NULL);
        vkFreeMemory(device, out->staging_memory, NULL);
        vkDestroySwapchainKHR(device, out->swapchain, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (out->instance) {
        vkDestroySurfaceKHR(out->instance, out->surface, NULL);
        vkDestroyInstance(out->instance, NULL);
    }
    free(out);
}

int vulkan_output_draw(struct vulkan_output *out, int producer_fd, int *release_fd)
{
    if (!out || !pthread_equal(out->owner, pthread_self()) || out->failed)
        goto fail;
    if (out->awaiting_present || !release_fd || producer_fd < -1)
        goto retire;

    struct draw_resources *r = &out->draws[out->next_draw];
    /* Only bound in-flight resource reuse; never poll or synchronously wait on
     * this frame's producer sync-file. */
    if (!VK_OK(vkWaitForFences(out->device, 1, &r->submitted, VK_TRUE, wsi_timeout_ns)))
        goto retire;

    const bool wait_producer = producer_fd >= 0;
    if (wait_producer) {
        const VkImportSemaphoreFdInfoKHR import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = r->producer_done,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, .handleType = sync_fd, .fd = producer_fd,
        };
        if (!VK_OK(out->import_semaphore_fd(out->device, &import)))
            goto retire;
        producer_fd = -1; /* Vulkan owns it after import. */
    }

    if (!VK_OK(vkResetFences(out->device, 1, &r->submitted)))
        goto retire;
    const VkResult acquired = vkAcquireNextImageKHR(out->device, out->swapchain, wsi_timeout_ns,
                                                    r->image_available, VK_NULL_HANDLE, &out->image_index);
    if (acquired != VK_SUBOPTIMAL_KHR && !vk_ok(acquired, "vkAcquireNextImageKHR"))
        goto retire;

    const struct swap_image *image = &out->images[out->image_index];
    const VkSemaphore waits[] = { r->image_available, r->producer_done };
    /* Queue-family acquires use ALL_COMMANDS scope without synchronization2's
     * narrower ownership-transfer scopes. */
    const VkPipelineStageFlags stages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    };
    const VkSemaphore signals[] = { image->ready_to_present, r->staging_released };
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait_producer ? 2 : 1,
        .pWaitSemaphores = waits, .pWaitDstStageMask = stages, .commandBufferCount = 1, .pCommandBuffers = &image->draw,
        .signalSemaphoreCount = 2, .pSignalSemaphores = signals,
    };
    if (!VK_OK(vkQueueSubmit(out->queue, 1, &submit, r->submitted)))
        goto retire;

    const VkSemaphoreGetFdInfoKHR get_fd = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = r->staging_released, .handleType = sync_fd,
    };
    int fd;
    if (!VK_OK(out->get_semaphore_fd(out->device, &get_fd, &fd)))
        goto retire;
    *release_fd = fd; /* A successful SYNC_FD export may return -1 (already signaled). */
    out->awaiting_present = true;
    out->next_draw = (out->next_draw + 1) % FRAMES_IN_FLIGHT;
    return 0;

retire:
    out->failed = true;
fail:
    if (producer_fd >= 0)
        close(producer_fd);
    return -1;
}

int vulkan_output_present(struct vulkan_output *out, struct window_frame *frame)
{
    if (!frame)
        return -1;
    *frame = (struct window_frame){0};
    if (!out || !pthread_equal(out->owner, pthread_self()) || out->failed || !out->awaiting_present)
        return -1;
    out->awaiting_present = false;
    const bool track = out->get_past_timing && out->last_present_id < UINT32_MAX;
    const VkPresentTimeGOOGLE time = { .presentID = out->last_present_id + 1, .desiredPresentTime = 0 };
    const VkPresentTimesInfoGOOGLE times = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &time,
    };
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = track ? &times : NULL,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &out->images[out->image_index].ready_to_present,
        .swapchainCount = 1, .pSwapchains = &out->swapchain, .pImageIndices = &out->image_index,
    };
    const VkResult result = vkQueuePresentKHR(out->queue, &present);
    if (result != VK_SUBOPTIMAL_KHR && !vk_ok(result, "vkQueuePresentKHR")) {
        out->failed = true;
        return -1;
    }
    if (track) {
        out->last_present_id = time.presentID;
        *frame = (struct window_frame){ .id = time.presentID, .tracked = true };
    }
    return 0;
}

int vulkan_output_presentation_time(struct vulkan_output *out, uint64_t id, uint64_t *ns)
{
    if (!out || !ns || !out->get_past_timing || out->failed ||
        !pthread_equal(out->owner, pthread_self()) || !id || id > out->last_present_id ||
        out->last_present_id - id >= TIMING_CACHE_SIZE)
        return -1;
    const struct presentation_sample *sample = &out->timings[id % TIMING_CACHE_SIZE];
    if (sample->id != id) {
        /* The API returns each ID only once. Cache the whole batch for the
         * caller's other outstanding IDs, not just the one being polled. */
        VkPastPresentationTimingGOOGLE timings[TIMING_CACHE_SIZE];
        uint32_t count = TIMING_CACHE_SIZE;
        const VkResult result = out->get_past_timing(out->device, out->swapchain, &count, timings);
        if (result != VK_INCOMPLETE && !vk_ok(result, "vkGetPastPresentationTimingGOOGLE")) {
            out->get_past_timing = NULL;
            return -1;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t tag = timings[i].presentID;
            if (tag && tag <= out->last_present_id && out->last_present_id - tag < TIMING_CACHE_SIZE) {
                out->timings[tag % TIMING_CACHE_SIZE] = (struct presentation_sample){ tag, timings[i].actualPresentTime };
            }
        }
    }
    if (sample->id != id)
        return 0;
    if (!sample->actual_ns || sample->actual_ns > INT64_MAX)
        return -1;
    *ns = sample->actual_ns;
    return 1;
}
