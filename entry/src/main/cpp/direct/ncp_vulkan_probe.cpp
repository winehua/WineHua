#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "vulkan_probe_protocol.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <dlfcn.h>
#include <errno.h>
#include <hilog/log.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_D0"

namespace {

using winehua::direct::VulkanProbeResult;

struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkEnumerateInstanceVersion enumerateInstanceVersion = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerateInstanceExtensions = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilies = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage cmdClearColorImage = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidateMappedMemoryRanges = nullptr;
};

struct VulkanState {
    void* loader = nullptr;
    VulkanFunctions vk;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    ~VulkanState()
    {
        if (device) {
            if (fence && vk.destroyFence) vk.destroyFence(device, fence, nullptr);
            if (commandPool && vk.destroyCommandPool) vk.destroyCommandPool(device, commandPool, nullptr);
            if (staging && vk.destroyBuffer) vk.destroyBuffer(device, staging, nullptr);
            if (image && vk.destroyImage) vk.destroyImage(device, image, nullptr);
            if (stagingMemory && vk.freeMemory) vk.freeMemory(device, stagingMemory, nullptr);
            if (imageMemory && vk.freeMemory) vk.freeMemory(device, imageMemory, nullptr);
            if (vk.destroyDevice) vk.destroyDevice(device, nullptr);
        }
        if (instance && vk.destroyInstance) vk.destroyInstance(instance, nullptr);
        if (loader) dlclose(loader);
    }
};

uint64_t MonotonicMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool Fail(VulkanProbeResult& out, const char* stage, VkResult result = VK_SUCCESS)
{
    std::snprintf(out.stage, sizeof(out.stage), "%s", stage);
    out.vkResult = result;
    return false;
}

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t bits,
                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred)
{
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

bool RunProbe(VulkanProbeResult& out)
{
    VulkanState s;
    s.loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!s.loader) return Fail(out, "dlopen_libvulkan");
    s.vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(s.loader, "vkGetInstanceProcAddr"));
    if (!s.vk.getInstanceProcAddr) return Fail(out, "vkGetInstanceProcAddr");
    Dl_info loaderInfo{};
    if (dladdr(reinterpret_cast<void*>(s.vk.getInstanceProcAddr), &loaderInfo) && loaderInfo.dli_fname)
        std::snprintf(out.loaderPath, sizeof(out.loaderPath), "%s", loaderInfo.dli_fname);
    // A bundled guest loader would invalidate this native-system Vulkan gate.
    if (!out.loaderPath[0] || std::strstr(out.loaderPath, "/data/storage/") ||
        std::strstr(out.loaderPath, "/data/app/"))
        return Fail(out, "non_system_loader");
    const char* icd = std::getenv("VK_ICD_FILENAMES");
    const char* drivers = std::getenv("VK_DRIVER_FILES");
    std::snprintf(out.icdEnvironment, sizeof(out.icdEnvironment), "ICD=%s DRIVER=%s",
                  icd ? icd : "", drivers ? drivers : "");
    OH_LOG_INFO(LOG_APP, "[DIRECT-D0] env=%{public}s LD_LIBRARY_PATH=%{public}s",
                out.icdEnvironment, std::getenv("LD_LIBRARY_PATH") ? std::getenv("LD_LIBRARY_PATH") : "");

#define LOAD_GLOBAL(field, name) do { \
    s.vk.field = reinterpret_cast<PFN_##name>(s.vk.getInstanceProcAddr(VK_NULL_HANDLE, #name)); \
    if (!s.vk.field) return Fail(out, #name); \
} while (0)
#define LOAD_INSTANCE(field, name) do { \
    s.vk.field = reinterpret_cast<PFN_##name>(s.vk.getInstanceProcAddr(s.instance, #name)); \
    if (!s.vk.field) return Fail(out, #name); \
} while (0)
#define LOAD_DEVICE(field, name) do { \
    s.vk.field = reinterpret_cast<PFN_##name>(s.vk.getDeviceProcAddr(s.device, #name)); \
    if (!s.vk.field) return Fail(out, #name); \
} while (0)

    LOAD_GLOBAL(createInstance, vkCreateInstance);
    LOAD_GLOBAL(enumerateInstanceExtensions, vkEnumerateInstanceExtensionProperties);
    s.vk.enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        s.vk.getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    out.loaderVersion = VK_API_VERSION_1_0;
    if (s.vk.enumerateInstanceVersion)
        s.vk.enumerateInstanceVersion(&out.loaderVersion);
    uint32_t extensionCount = 0;
    const VkResult extensionResult = s.vk.enumerateInstanceExtensions(nullptr, &extensionCount, nullptr);
    if (extensionResult == VK_SUCCESS) out.instanceExtensionCount = extensionCount;
    OH_LOG_INFO(LOG_APP, "[DIRECT-D0] loaderApi=%{public}u extResult=%{public}d extCount=%{public}u",
                out.loaderVersion, extensionResult, extensionCount);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WineHua Direct D0";
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
    const uint32_t requestedVersions[] = {
        VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3
    };
    for (const uint32_t version : requestedVersions) {
        if (version > out.loaderVersion) break;
        app.apiVersion = version;
        result = s.vk.createInstance(&instanceInfo, nullptr, &s.instance);
        OH_LOG_INFO(LOG_APP, "[DIRECT-D0] vkCreateInstance api=%{public}u result=%{public}d",
                    version, result);
        if (result == VK_SUCCESS) {
            out.requestedApiVersion = version;
            break;
        }
    }
    if (result != VK_SUCCESS) return Fail(out, "vkCreateInstance", result);

    LOAD_INSTANCE(destroyInstance, vkDestroyInstance);
    LOAD_INSTANCE(enumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(getPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(getQueueFamilies, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(getMemoryProperties, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(createDevice, vkCreateDevice);
    LOAD_INSTANCE(getDeviceProcAddr, vkGetDeviceProcAddr);

    uint32_t physicalCount = 0;
    result = s.vk.enumeratePhysicalDevices(s.instance, &physicalCount, nullptr);
    if (result != VK_SUCCESS || !physicalCount) return Fail(out, "vkEnumeratePhysicalDevices", result);
    // A bounded list is enough for the probe; fail explicitly if a device behaves unexpectedly.
    if (physicalCount > 16) return Fail(out, "physical_device_count");
    VkPhysicalDevice physicals[16]{};
    result = s.vk.enumeratePhysicalDevices(s.instance, &physicalCount, physicals);
    if (result != VK_SUCCESS) return Fail(out, "vkEnumeratePhysicalDevices", result);
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < physicalCount && !physical; ++i) {
        VkPhysicalDeviceProperties properties{};
        s.vk.getPhysicalDeviceProperties(physicals[i], &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        uint32_t familyCount = 0;
        s.vk.getQueueFamilies(physicals[i], &familyCount, nullptr);
        if (!familyCount || familyCount > 32) continue;
        VkQueueFamilyProperties families[32]{};
        s.vk.getQueueFamilies(physicals[i], &familyCount, families);
        for (uint32_t j = 0; j < familyCount; ++j) {
            if (families[j].queueCount && (families[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                physical = physicals[i];
                queueFamily = j;
                out.apiVersion = properties.apiVersion;
                std::snprintf(out.deviceName, sizeof(out.deviceName), "%s", properties.deviceName);
                break;
            }
        }
    }
    if (!physical) return Fail(out, "native_graphics_device");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    result = s.vk.createDevice(physical, &deviceInfo, nullptr, &s.device);
    if (result != VK_SUCCESS) return Fail(out, "vkCreateDevice", result);

    LOAD_DEVICE(destroyDevice, vkDestroyDevice);
    LOAD_DEVICE(getDeviceQueue, vkGetDeviceQueue);
    LOAD_DEVICE(createImage, vkCreateImage);
    LOAD_DEVICE(destroyImage, vkDestroyImage);
    LOAD_DEVICE(getImageMemoryRequirements, vkGetImageMemoryRequirements);
    LOAD_DEVICE(createBuffer, vkCreateBuffer);
    LOAD_DEVICE(destroyBuffer, vkDestroyBuffer);
    LOAD_DEVICE(getBufferMemoryRequirements, vkGetBufferMemoryRequirements);
    LOAD_DEVICE(allocateMemory, vkAllocateMemory);
    LOAD_DEVICE(freeMemory, vkFreeMemory);
    LOAD_DEVICE(bindImageMemory, vkBindImageMemory);
    LOAD_DEVICE(bindBufferMemory, vkBindBufferMemory);
    LOAD_DEVICE(createCommandPool, vkCreateCommandPool);
    LOAD_DEVICE(destroyCommandPool, vkDestroyCommandPool);
    LOAD_DEVICE(allocateCommandBuffers, vkAllocateCommandBuffers);
    LOAD_DEVICE(beginCommandBuffer, vkBeginCommandBuffer);
    LOAD_DEVICE(endCommandBuffer, vkEndCommandBuffer);
    LOAD_DEVICE(cmdPipelineBarrier, vkCmdPipelineBarrier);
    LOAD_DEVICE(cmdClearColorImage, vkCmdClearColorImage);
    LOAD_DEVICE(cmdCopyImageToBuffer, vkCmdCopyImageToBuffer);
    LOAD_DEVICE(createFence, vkCreateFence);
    LOAD_DEVICE(destroyFence, vkDestroyFence);
    LOAD_DEVICE(queueSubmit, vkQueueSubmit);
    LOAD_DEVICE(waitForFences, vkWaitForFences);
    LOAD_DEVICE(mapMemory, vkMapMemory);
    LOAD_DEVICE(unmapMemory, vkUnmapMemory);
    LOAD_DEVICE(invalidateMappedMemoryRanges, vkInvalidateMappedMemoryRanges);

    VkQueue queue = VK_NULL_HANDLE;
    s.vk.getDeviceQueue(s.device, queueFamily, 0, &queue);
    if (!queue) return Fail(out, "vkGetDeviceQueue");
    VkPhysicalDeviceMemoryProperties memoryProps{};
    s.vk.getMemoryProperties(physical, &memoryProps);

    constexpr uint32_t width = 16;
    constexpr uint32_t height = 16;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = s.vk.createImage(s.device, &imageInfo, nullptr, &s.image);
    if (result != VK_SUCCESS) return Fail(out, "vkCreateImage", result);
    VkMemoryRequirements imageReq{};
    s.vk.getImageMemoryRequirements(s.device, s.image, &imageReq);
    uint32_t imageType = FindMemoryType(memoryProps, imageReq.memoryTypeBits, 0,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageType == UINT32_MAX) return Fail(out, "image_memory_type");
    VkMemoryAllocateInfo imageAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = imageType;
    result = s.vk.allocateMemory(s.device, &imageAlloc, nullptr, &s.imageMemory);
    if (result != VK_SUCCESS) return Fail(out, "vkAllocateMemory_image", result);
    result = s.vk.bindImageMemory(s.device, s.image, s.imageMemory, 0);
    if (result != VK_SUCCESS) return Fail(out, "vkBindImageMemory", result);

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = width * height * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = s.vk.createBuffer(s.device, &bufferInfo, nullptr, &s.staging);
    if (result != VK_SUCCESS) return Fail(out, "vkCreateBuffer", result);
    VkMemoryRequirements bufferReq{};
    s.vk.getBufferMemoryRequirements(s.device, s.staging, &bufferReq);
    uint32_t bufferType = FindMemoryType(memoryProps, bufferReq.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bufferType == UINT32_MAX) return Fail(out, "staging_memory_type");
    VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufferAlloc.allocationSize = bufferReq.size;
    bufferAlloc.memoryTypeIndex = bufferType;
    result = s.vk.allocateMemory(s.device, &bufferAlloc, nullptr, &s.stagingMemory);
    if (result != VK_SUCCESS) return Fail(out, "vkAllocateMemory_staging", result);
    result = s.vk.bindBufferMemory(s.device, s.staging, s.stagingMemory, 0);
    if (result != VK_SUCCESS) return Fail(out, "vkBindBufferMemory", result);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamily;
    result = s.vk.createCommandPool(s.device, &poolInfo, nullptr, &s.commandPool);
    if (result != VK_SUCCESS) return Fail(out, "vkCreateCommandPool", result);
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = s.commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = s.vk.allocateCommandBuffers(s.device, &commandInfo, &command);
    if (result != VK_SUCCESS) return Fail(out, "vkAllocateCommandBuffers", result);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    result = s.vk.beginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS) return Fail(out, "vkBeginCommandBuffer", result);

    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = s.image;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.layerCount = 1;
    s.vk.cmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                            1, &imageBarrier);
    VkClearColorValue clear{};
    clear.float32[0] = 1.0f;
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    s.vk.cmdClearColorImage(command, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &clear, 1, &range);
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    s.vk.cmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                            1, &imageBarrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    s.vk.cmdCopyImageToBuffer(command, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              s.staging, 1, &copy);
    VkBufferMemoryBarrier bufferBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = s.staging;
    bufferBarrier.size = VK_WHOLE_SIZE;
    s.vk.cmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bufferBarrier,
                            0, nullptr);
    result = s.vk.endCommandBuffer(command);
    if (result != VK_SUCCESS) return Fail(out, "vkEndCommandBuffer", result);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = s.vk.createFence(s.device, &fenceInfo, nullptr, &s.fence);
    if (result != VK_SUCCESS) return Fail(out, "vkCreateFence", result);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = s.vk.queueSubmit(queue, 1, &submit, s.fence);
    if (result != VK_SUCCESS) return Fail(out, "vkQueueSubmit", result);
    result = s.vk.waitForFences(s.device, 1, &s.fence, VK_TRUE, 5'000'000'000ULL);
    if (result != VK_SUCCESS) return Fail(out, "vkWaitForFences", result);

    void* mapped = nullptr;
    result = s.vk.mapMemory(s.device, s.stagingMemory, 0, bufferReq.size, 0, &mapped);
    if (result != VK_SUCCESS) return Fail(out, "vkMapMemory", result);
    if (!(memoryProps.memoryTypes[bufferType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange mappedRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mappedRange.memory = s.stagingMemory;
        mappedRange.size = VK_WHOLE_SIZE;
        result = s.vk.invalidateMappedMemoryRanges(s.device, 1, &mappedRange);
    }
    if (result == VK_SUCCESS) {
        const auto* pixels = static_cast<const uint8_t*>(mapped);
        out.pixelCheck = 1;
        for (uint32_t i = 0; i < width * height; ++i) {
            const uint8_t* p = pixels + i * 4;
            if (p[0] != 255 || p[1] != 0 || p[2] != 0 || p[3] != 255) {
                out.pixelCheck = 0;
                break;
            }
        }
    }
    s.vk.unmapMemory(s.device, s.stagingMemory);
    if (result != VK_SUCCESS) return Fail(out, "vkInvalidateMappedMemoryRanges", result);
    if (!out.pixelCheck) return Fail(out, "pixel_mismatch");
    std::snprintf(out.stage, sizeof(out.stage), "complete");
    out.vkResult = VK_SUCCESS;
    return true;

#undef LOAD_GLOBAL
#undef LOAD_INSTANCE
#undef LOAD_DEVICE
}

void WriteResult(int fd, const VulkanProbeResult& result)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&result);
    size_t remaining = sizeof(result);
    while (remaining) {
        const ssize_t count = write(fd, bytes, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        bytes += count;
        remaining -= static_cast<size_t>(count);
    }
}

} // namespace

namespace {

std::mutex g_createMutex;
std::condition_variable g_createCondition;
VulkanProbeResult g_createResult{};
bool g_createReady = false;
bool g_createFinished = false;

int OnDiagnosticRequest(uint32_t code, const OHIPCParcel* data, OHIPCParcel* reply, void*)
{
    int32_t version = 0;
    if (!data || OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS ||
        version != static_cast<int32_t>(winehua::direct::kVulkanProbeVersion))
        return OH_IPC_CHECK_PARAM_ERROR;
    if (code == winehua::direct::kVulkanProbeFinishRequest) {
        {
            std::lock_guard<std::mutex> lock(g_createMutex);
            g_createFinished = true;
        }
        g_createCondition.notify_all();
        return reply ? OH_IPCParcel_WriteInt32(reply, 0) : OH_IPC_CHECK_PARAM_ERROR;
    }
    if (code != winehua::direct::kVulkanProbeReadRequest || !reply)
        return OH_IPC_CHECK_PARAM_ERROR;
    std::unique_lock<std::mutex> lock(g_createMutex);
    if (!g_createCondition.wait_for(lock, std::chrono::seconds(15), [] { return g_createReady; }))
        return OH_IPC_INNER_ERROR;
    return OH_IPCParcel_WriteBuffer(reply,
        reinterpret_cast<const uint8_t*>(&g_createResult), sizeof(g_createResult));
}

} // namespace

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    return OH_IPCRemoteStub_Create("winehua.direct.D0", OnDiagnosticRequest, nullptr, nullptr);
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    VulkanProbeResult result{};
    result.pid = getpid();
    const uint64_t started = MonotonicMs();
    const bool passed = RunProbe(result);
    result.elapsedMs = MonotonicMs() - started;
    result.status = passed ? 0 : -1;
    {
        std::lock_guard<std::mutex> lock(g_createMutex);
        g_createResult = result;
        g_createReady = true;
    }
    g_createCondition.notify_all();
    OH_LOG_INFO(LOG_APP,
                "[DIRECT-D0-CREATE] status=%{public}d stage=%{public}s pid=%{public}d "
                "device=%{public}s loader=%{public}s vk=%{public}d pixel=%{public}u ms=%{public}llu",
                passed ? 0 : -1, result.stage, result.pid, result.deviceName,
                result.loaderPath, result.vkResult, result.pixelCheck,
                static_cast<unsigned long long>(result.elapsedMs));
    std::unique_lock<std::mutex> lock(g_createMutex);
    g_createCondition.wait_for(lock, std::chrono::seconds(15), [] { return g_createFinished; });
}

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    VulkanProbeResult result{};
    result.pid = getpid();
    const uint64_t started = MonotonicMs();
    int outputFd = -1;
    for (NativeChildProcess_Fd* item = args.fdList.head; item; item = item->next) {
        if (item->fdName && std::strcmp(item->fdName, winehua::direct::kVulkanProbeFdName) == 0) {
            outputFd = item->fd;
            break;
        }
    }
    const bool passed = RunProbe(result);
    result.status = passed ? 0 : -1;
    result.elapsedMs = MonotonicMs() - started;
    OH_LOG_INFO(LOG_APP,
                "[DIRECT-D0] status=%{public}d stage=%{public}s pid=%{public}d "
                "device=%{public}s loader=%{public}s vk=%{public}d pixels=%{public}u "
                "loaderApi=%{public}u requestApi=%{public}u ext=%{public}u ms=%{public}llu",
                result.status, result.stage, result.pid, result.deviceName, result.loaderPath,
                result.vkResult, result.pixelCheck, result.loaderVersion,
                result.requestedApiVersion, result.instanceExtensionCount,
                static_cast<unsigned long long>(result.elapsedMs));
    if (outputFd >= 0) {
        WriteResult(outputFd, result);
        close(outputFd);
    }
}
