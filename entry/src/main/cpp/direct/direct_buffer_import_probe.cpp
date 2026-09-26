#include "direct_buffer_import_probe.h"

#include <native_buffer/native_buffer.h>

#include <algorithm>
#include <cstring>

namespace winehua::direct {

DirectBufferImportProbe::~DirectBufferImportProbe()
{
    ClearCache();
    if (device_) vkDestroyDevice(device_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool DirectBufferImportProbe::Fail(const char* stage, VkResult result)
{
    stage_ = stage;
    error_ = result;
    return false;
}

void DirectBufferImportProbe::ClearCache()
{
    for (auto& [sequence, entry] : cache_) {
        (void)sequence;
        if (entry.view) vkDestroyImageView(device_, entry.view, nullptr);
        if (entry.image) vkDestroyImage(device_, entry.image, nullptr);
        if (entry.memory) vkFreeMemory(device_, entry.memory, nullptr);
        if (entry.buffer) OH_NativeBuffer_Unreference(entry.buffer);
    }
    cache_.clear();
}

void DirectBufferImportProbe::NewGeneration()
{
    ClearCache();
}

bool DirectBufferImportProbe::Initialize()
{
    if (device_) return true;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WineHua Direct D2 import";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) return Fail("import_instance", result);
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (result != VK_SUCCESS || !count || count > 16)
        return Fail("import_physical_count",
                    result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
    VkPhysicalDevice devices[16]{};
    result = vkEnumeratePhysicalDevices(instance_, &count, devices);
    if (result != VK_SUCCESS) return Fail("import_physical", result);
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            physical_ = devices[i];
            break;
        }
    }
    if (!physical_) return Fail("import_native_device", VK_ERROR_INITIALIZATION_FAILED);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
    if (!familyCount || familyCount > 32)
        return Fail("import_queue_count", VK_ERROR_INITIALIZATION_FAILED);
    VkQueueFamilyProperties families[32]{};
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) return Fail("import_queue", VK_ERROR_INITIALIZATION_FAILED);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* extensions[] = {
        VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME,
        "VK_EXT_queue_family_foreign",
    };
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = extensions;
    result = vkCreateDevice(physical_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) return Fail("import_device", result);
    return true;
}

bool DirectBufferImportProbe::Import(OH_NativeBuffer* buffer, int32_t width, int32_t height)
{
    if (!buffer || width <= 0 || height <= 0)
        return Fail("import_input", VK_ERROR_INITIALIZATION_FAILED);
    if (!Initialize()) return false;
    const uint32_t sequence = OH_NativeBuffer_GetSeqNum(buffer);
    auto found = cache_.find(sequence);
    if (found != cache_.end()) {
        if (found->second.buffer != buffer)
            return Fail("import_seq_alias", VK_ERROR_INITIALIZATION_FAILED);
        ++reuses_;
        stage_ = "reused";
        return true;
    }

    VkNativeBufferFormatPropertiesOHOS format{
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS properties{VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    properties.pNext = &format;
    VkResult result = vkGetNativeBufferPropertiesOHOS(device_, buffer, &properties);
    if (result != VK_SUCCESS) return Fail("native_buffer_properties", result);
    if (format.format != VK_FORMAT_R8G8B8A8_UNORM || !properties.allocationSize ||
        !properties.memoryTypeBits)
        return Fail("native_buffer_format", VK_ERROR_FORMAT_NOT_SUPPORTED);

    Entry entry{};
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format.format;
    imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(device_, &imageInfo, nullptr, &entry.image);
    if (result != VK_SUCCESS) return Fail("import_image", result);
    auto cleanup = [&] {
        if (entry.view) vkDestroyImageView(device_, entry.view, nullptr);
        if (entry.image) vkDestroyImage(device_, entry.image, nullptr);
        if (entry.memory) vkFreeMemory(device_, entry.memory, nullptr);
    };
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, entry.image, &requirements);
    const uint32_t compatibleTypes = properties.memoryTypeBits & requirements.memoryTypeBits;
    if (!compatibleTypes) {
        cleanup();
        return Fail("import_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    uint32_t typeIndex = 0;
    while (!(compatibleTypes & (1u << typeIndex))) ++typeIndex;
    VkImportNativeBufferInfoOHOS nativeImport{VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
    nativeImport.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &nativeImport;
    dedicated.image = entry.image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &dedicated;
    allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = typeIndex;
    result = vkAllocateMemory(device_, &allocation, nullptr, &entry.memory);
    if (result != VK_SUCCESS) {
        cleanup();
        return Fail("import_allocate", result);
    }
    result = vkBindImageMemory(device_, entry.image, entry.memory, 0);
    if (result != VK_SUCCESS) {
        cleanup();
        return Fail("import_bind", result);
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = entry.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device_, &viewInfo, nullptr, &entry.view);
    if (result != VK_SUCCESS) {
        cleanup();
        return Fail("import_view", result);
    }
    if (OH_NativeBuffer_Reference(buffer) != 0) {
        cleanup();
        return Fail("import_reference", VK_ERROR_INITIALIZATION_FAILED);
    }
    entry.buffer = buffer;
    cache_.emplace(sequence, entry);
    ++imports_;
    stage_ = "imported";
    error_ = VK_SUCCESS;
    return true;
}

} // namespace winehua::direct
