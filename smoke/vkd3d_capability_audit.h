#ifndef WINEHUA_VKD3D_CAPABILITY_AUDIT_H
#define WINEHUA_VKD3D_CAPABILITY_AUDIT_H

/* Gate A records queried values only. It does not enable features or create a device. */

#include <stdarg.h>

struct winehua_vkd3d_json_builder {
    char *data;
    size_t capacity;
    size_t length;
    int failed;
};

static int winehua_vkd3d_json_append(struct winehua_vkd3d_json_builder *builder,
                                     const char *format, ...)
{
    va_list args;
    int written;
    if (builder->failed) return 0;
    va_start(args, format);
    written = vsnprintf(builder->data + builder->length,
                        builder->capacity - builder->length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= builder->capacity - builder->length) {
        builder->failed = 1;
        return 0;
    }
    builder->length += (size_t)written;
    return 1;
}

static const char *winehua_vkd3d_bool(VkBool32 value)
{
    return value ? "true" : "false";
}

static int winehua_vkd3d_append_descriptor_features(
    struct winehua_vkd3d_json_builder *builder,
    const VkPhysicalDeviceVulkan12Features *features)
{
    return winehua_vkd3d_json_append(builder,
        "{\"descriptorIndexing\":%s,"
        "\"shaderInputAttachmentArrayDynamicIndexing\":%s,"
        "\"shaderUniformTexelBufferArrayDynamicIndexing\":%s,"
        "\"shaderStorageTexelBufferArrayDynamicIndexing\":%s,"
        "\"shaderUniformBufferArrayNonUniformIndexing\":%s,"
        "\"shaderSampledImageArrayNonUniformIndexing\":%s,"
        "\"shaderStorageBufferArrayNonUniformIndexing\":%s,"
        "\"shaderStorageImageArrayNonUniformIndexing\":%s,"
        "\"shaderInputAttachmentArrayNonUniformIndexing\":%s,"
        "\"shaderUniformTexelBufferArrayNonUniformIndexing\":%s,"
        "\"shaderStorageTexelBufferArrayNonUniformIndexing\":%s,"
        "\"descriptorBindingUniformBufferUpdateAfterBind\":%s,"
        "\"descriptorBindingSampledImageUpdateAfterBind\":%s,"
        "\"descriptorBindingStorageImageUpdateAfterBind\":%s,"
        "\"descriptorBindingStorageBufferUpdateAfterBind\":%s,"
        "\"descriptorBindingUniformTexelBufferUpdateAfterBind\":%s,"
        "\"descriptorBindingStorageTexelBufferUpdateAfterBind\":%s,"
        "\"descriptorBindingUpdateUnusedWhilePending\":%s,"
        "\"descriptorBindingPartiallyBound\":%s,"
        "\"descriptorBindingVariableDescriptorCount\":%s,"
        "\"runtimeDescriptorArray\":%s}",
        winehua_vkd3d_bool(features->descriptorIndexing),
        winehua_vkd3d_bool(features->shaderInputAttachmentArrayDynamicIndexing),
        winehua_vkd3d_bool(features->shaderUniformTexelBufferArrayDynamicIndexing),
        winehua_vkd3d_bool(features->shaderStorageTexelBufferArrayDynamicIndexing),
        winehua_vkd3d_bool(features->shaderUniformBufferArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->shaderSampledImageArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->shaderStorageBufferArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->shaderStorageImageArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->shaderInputAttachmentArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->shaderUniformTexelBufferArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->shaderStorageTexelBufferArrayNonUniformIndexing),
        winehua_vkd3d_bool(features->descriptorBindingUniformBufferUpdateAfterBind),
        winehua_vkd3d_bool(features->descriptorBindingSampledImageUpdateAfterBind),
        winehua_vkd3d_bool(features->descriptorBindingStorageImageUpdateAfterBind),
        winehua_vkd3d_bool(features->descriptorBindingStorageBufferUpdateAfterBind),
        winehua_vkd3d_bool(features->descriptorBindingUniformTexelBufferUpdateAfterBind),
        winehua_vkd3d_bool(features->descriptorBindingStorageTexelBufferUpdateAfterBind),
        winehua_vkd3d_bool(features->descriptorBindingUpdateUnusedWhilePending),
        winehua_vkd3d_bool(features->descriptorBindingPartiallyBound),
        winehua_vkd3d_bool(features->descriptorBindingVariableDescriptorCount),
        winehua_vkd3d_bool(features->runtimeDescriptorArray));
}

static int winehua_vkd3d_append_format(struct winehua_vkd3d_json_builder *builder,
                                       VkPhysicalDevice physical, const char *name,
                                       VkFormat format, int comma)
{
    VkFormatProperties properties;
    VkFormatFeatureFlags flags;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    flags = properties.optimalTilingFeatures;
    return winehua_vkd3d_json_append(builder,
        "%s\"%s\":{\"optimalTilingFeatures\":%u,\"sampledImage\":%s,"
        "\"storageImage\":%s,\"colorAttachment\":%s,\"depthStencilAttachment\":%s}",
        comma ? "," : "", name, flags,
        winehua_vkd3d_bool(flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
        winehua_vkd3d_bool(flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),
        winehua_vkd3d_bool(flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT),
        winehua_vkd3d_bool(flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
}

static char *winehua_vkd3d_capability_audit(
    VkPhysicalDevice physical, const VkExtensionProperties *extensions,
    uint32_t extension_count, const VkPhysicalDeviceVulkan11Features *features11,
    const VkPhysicalDeviceVulkan12Features *features12,
    const VkPhysicalDeviceVulkan13Features *features13,
    const VkPhysicalDeviceVulkan12Properties *properties12,
    const VkPhysicalDeviceIDProperties *id_properties)
{
    struct winehua_vkd3d_json_builder builder;
    VkPhysicalDeviceMemoryProperties memory;
    VkQueueFamilyProperties *queues = NULL;
    uint32_t queue_count = 0;
    uint32_t i;

    builder.capacity = 65536;
    builder.length = 0;
    builder.failed = 0;
    builder.data = (char *)calloc(builder.capacity, 1);
    if (!builder.data) return NULL;
    winehua_vkd3d_json_append(&builder, "{\"deviceExtensions\":[");
    for (i = 0; i < extension_count; ++i)
        winehua_vkd3d_json_append(&builder, "%s{\"name\":\"%s\",\"specVersion\":%u}",
                                  i ? "," : "", extensions[i].extensionName,
                                  extensions[i].specVersion);
    winehua_vkd3d_json_append(&builder, "],\"descriptorIndexingFeatures\":");
    winehua_vkd3d_append_descriptor_features(&builder, features12);
    winehua_vkd3d_json_append(&builder,
        ",\"featureChain\":{\"vulkan11\":{\"shaderDrawParameters\":%s},"
        "\"vulkan12\":{\"timelineSemaphore\":%s,"
        "\"bufferDeviceAddress\":%s,\"bufferDeviceAddressCaptureReplay\":%s,"
        "\"bufferDeviceAddressMultiDevice\":%s,\"samplerMirrorClampToEdge\":%s,"
        "\"scalarBlockLayout\":%s},"
        "\"vulkan13\":{\"synchronization2\":%s,\"dynamicRendering\":%s,"
        "\"maintenance4\":%s}},",
        winehua_vkd3d_bool(features11->shaderDrawParameters),
        winehua_vkd3d_bool(features12->timelineSemaphore),
        winehua_vkd3d_bool(features12->bufferDeviceAddress),
        winehua_vkd3d_bool(features12->bufferDeviceAddressCaptureReplay),
        winehua_vkd3d_bool(features12->bufferDeviceAddressMultiDevice),
        winehua_vkd3d_bool(features12->samplerMirrorClampToEdge),
        winehua_vkd3d_bool(features12->scalarBlockLayout),
        winehua_vkd3d_bool(features13->synchronization2),
        winehua_vkd3d_bool(features13->dynamicRendering),
        winehua_vkd3d_bool(features13->maintenance4));
    winehua_vkd3d_json_append(&builder,
        "\"updateAfterBindLimits\":{\"maxUpdateAfterBindDescriptorsInAllPools\":%u,"
        "\"maxPerStageDescriptorUpdateAfterBindSamplers\":%u,"
        "\"maxPerStageDescriptorUpdateAfterBindUniformBuffers\":%u,"
        "\"maxPerStageDescriptorUpdateAfterBindStorageBuffers\":%u,"
        "\"maxPerStageDescriptorUpdateAfterBindSampledImages\":%u,"
        "\"maxPerStageDescriptorUpdateAfterBindStorageImages\":%u,"
        "\"maxPerStageDescriptorUpdateAfterBindInputAttachments\":%u,"
        "\"maxPerStageUpdateAfterBindResources\":%u,"
        "\"maxDescriptorSetUpdateAfterBindSamplers\":%u,"
        "\"maxDescriptorSetUpdateAfterBindUniformBuffers\":%u,"
        "\"maxDescriptorSetUpdateAfterBindUniformBuffersDynamic\":%u,"
        "\"maxDescriptorSetUpdateAfterBindStorageBuffers\":%u,"
        "\"maxDescriptorSetUpdateAfterBindStorageBuffersDynamic\":%u,"
        "\"maxDescriptorSetUpdateAfterBindSampledImages\":%u,"
        "\"maxDescriptorSetUpdateAfterBindStorageImages\":%u,"
        "\"maxDescriptorSetUpdateAfterBindInputAttachments\":%u},",
        properties12->maxUpdateAfterBindDescriptorsInAllPools,
        properties12->maxPerStageDescriptorUpdateAfterBindSamplers,
        properties12->maxPerStageDescriptorUpdateAfterBindUniformBuffers,
        properties12->maxPerStageDescriptorUpdateAfterBindStorageBuffers,
        properties12->maxPerStageDescriptorUpdateAfterBindSampledImages,
        properties12->maxPerStageDescriptorUpdateAfterBindStorageImages,
        properties12->maxPerStageDescriptorUpdateAfterBindInputAttachments,
        properties12->maxPerStageUpdateAfterBindResources,
        properties12->maxDescriptorSetUpdateAfterBindSamplers,
        properties12->maxDescriptorSetUpdateAfterBindUniformBuffers,
        properties12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic,
        properties12->maxDescriptorSetUpdateAfterBindStorageBuffers,
        properties12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic,
        properties12->maxDescriptorSetUpdateAfterBindSampledImages,
        properties12->maxDescriptorSetUpdateAfterBindStorageImages,
        properties12->maxDescriptorSetUpdateAfterBindInputAttachments);
    winehua_vkd3d_json_append(&builder,
        "\"propertyChain\":{\"vulkan12\":{\"driverId\":%u,"
        "\"maxUpdateAfterBindDescriptorsInAllPools\":%u,"
        "\"shaderUniformBufferArrayNonUniformIndexingNative\":%s,"
        "\"shaderSampledImageArrayNonUniformIndexingNative\":%s,"
        "\"shaderStorageBufferArrayNonUniformIndexingNative\":%s,"
        "\"shaderStorageImageArrayNonUniformIndexingNative\":%s,"
        "\"shaderInputAttachmentArrayNonUniformIndexingNative\":%s,"
        "\"robustBufferAccessUpdateAfterBind\":%s,\"quadDivergentImplicitLod\":%s}},",
        properties12->driverID, properties12->maxUpdateAfterBindDescriptorsInAllPools,
        winehua_vkd3d_bool(properties12->shaderUniformBufferArrayNonUniformIndexingNative),
        winehua_vkd3d_bool(properties12->shaderSampledImageArrayNonUniformIndexingNative),
        winehua_vkd3d_bool(properties12->shaderStorageBufferArrayNonUniformIndexingNative),
        winehua_vkd3d_bool(properties12->shaderStorageImageArrayNonUniformIndexingNative),
        winehua_vkd3d_bool(properties12->shaderInputAttachmentArrayNonUniformIndexingNative),
        winehua_vkd3d_bool(properties12->robustBufferAccessUpdateAfterBind),
        winehua_vkd3d_bool(properties12->quadDivergentImplicitLod));
    winehua_vkd3d_json_append(&builder, "\"deviceUuid\":\"");
    for (i = 0; i < VK_UUID_SIZE; ++i)
        winehua_vkd3d_json_append(&builder, "%02x", id_properties->deviceUUID[i]);
    winehua_vkd3d_json_append(&builder, "\",\"driverUuid\":\"");
    for (i = 0; i < VK_UUID_SIZE; ++i)
        winehua_vkd3d_json_append(&builder, "%02x", id_properties->driverUUID[i]);
    winehua_vkd3d_json_append(&builder, "\",\"queues\":[");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    queues = (VkQueueFamilyProperties *)calloc(queue_count ? queue_count : 1, sizeof(*queues));
    if (queues) vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues);
    for (i = 0; queues && i < queue_count; ++i)
        winehua_vkd3d_json_append(&builder,
            "%s{\"index\":%u,\"flags\":%u,\"queueCount\":%u,\"timestampValidBits\":%u,"
            "\"minImageTransferGranularity\":[%u,%u,%u]}", i ? "," : "", i,
            queues[i].queueFlags, queues[i].queueCount, queues[i].timestampValidBits,
            queues[i].minImageTransferGranularity.width, queues[i].minImageTransferGranularity.height,
            queues[i].minImageTransferGranularity.depth);
    free(queues);
    winehua_vkd3d_json_append(&builder, "],\"memory\":{");
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    winehua_vkd3d_json_append(&builder, "\"types\":[");
    for (i = 0; i < memory.memoryTypeCount; ++i)
        winehua_vkd3d_json_append(&builder, "%s{\"index\":%u,\"propertyFlags\":%u,\"heapIndex\":%u}",
                                  i ? "," : "", i, memory.memoryTypes[i].propertyFlags,
                                  memory.memoryTypes[i].heapIndex);
    winehua_vkd3d_json_append(&builder, "],\"heaps\":[");
    for (i = 0; i < memory.memoryHeapCount; ++i)
        winehua_vkd3d_json_append(&builder, "%s{\"index\":%u,\"size\":%llu,\"flags\":%u}",
                                  i ? "," : "", i,
                                  (unsigned long long)memory.memoryHeaps[i].size,
                                  memory.memoryHeaps[i].flags);
    winehua_vkd3d_json_append(&builder, "]},\"formats\":{");
    winehua_vkd3d_append_format(&builder, physical, "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, 0);
    winehua_vkd3d_append_format(&builder, physical, "D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT, 1);
    winehua_vkd3d_append_format(&builder, physical, "BC1_RGBA_UNORM_BLOCK", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1);
    winehua_vkd3d_append_format(&builder, physical, "BC7_UNORM_BLOCK", VK_FORMAT_BC7_UNORM_BLOCK, 1);
    winehua_vkd3d_json_append(&builder, "}}");
    if (builder.failed) {
        free(builder.data);
        return NULL;
    }
    return builder.data;
}

#endif
