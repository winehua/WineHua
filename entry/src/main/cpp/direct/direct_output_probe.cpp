#define VK_USE_PLATFORM_OHOS 1
#include "direct_output_probe.h"

#include <native_window/external_window.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <dirent.h>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <vector>

namespace winehua::direct {
namespace {

std::mutex g_surfaceMutex;
std::condition_variable g_surfaceReady;
uint64_t g_surfaceId = 0;
uint32_t g_surfaceWidth = 0;
uint32_t g_surfaceHeight = 0;
uint64_t g_surfaceSizeGeneration = 0;
bool g_resizeRequested = false;
std::atomic<uint32_t> g_runSequence{0};

int CountOpenFds()
{
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (const dirent* entry = readdir(directory))
        if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count - 1;
}

int ReadRssKiB()
{
    FILE* statm = std::fopen("/proc/self/statm", "r");
    if (!statm) return -1;
    unsigned long pages = 0;
    unsigned long resident = 0;
    const int scanned = std::fscanf(statm, "%lu %lu", &pages, &resident);
    std::fclose(statm);
    return scanned == 2 ? static_cast<int>(resident * sysconf(_SC_PAGESIZE) / 1024) : -1;
}

struct OutputWork {
    napi_async_work asyncWork = nullptr;
    napi_deferred deferred = nullptr;
    const char* stage = "pending";
    VkResult error = VK_SUCCESS;
    uint32_t frames = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t sequence = 0;
    uint64_t surfaceId = 0;
};

struct OutputVulkan {
    OHNativeWindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;
    VkFence submitted = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t queueFamily = UINT32_MAX;

    ~OutputVulkan()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            if (submitted) vkDestroyFence(device, submitted, nullptr);
            for (VkSemaphore semaphore : rendered)
                if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
            if (acquired) vkDestroySemaphore(device, acquired, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window) OH_NativeWindow_DestroyNativeWindow(window);
    }

    bool Fail(OutputWork& work, const char* where, VkResult result)
    {
        work.stage = where;
        work.error = result;
        return false;
    }

    bool Initialize(OutputWork& work, uint64_t surfaceId)
    {
        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window) != 0 || !window)
            return Fail(work, "output_window", VK_ERROR_INITIALIZATION_FAILED);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WineHua Direct D2 output";
        app.apiVersion = VK_API_VERSION_1_1;
        const char* instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
        if (result != VK_SUCCESS) return Fail(work, "output_instance", result);
        VkSurfaceCreateInfoOHOS surfaceInfo{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
        surfaceInfo.window = window;
        result = vkCreateSurfaceOHOS(instance, &surfaceInfo, nullptr, &surface);
        if (result != VK_SUCCESS) return Fail(work, "output_surface", result);

        uint32_t deviceCount = 0;
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (result != VK_SUCCESS || !deviceCount || deviceCount > 16)
            return Fail(work, "output_physical_count",
                        result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
        VkPhysicalDevice devices[16]{};
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices);
        if (result != VK_SUCCESS) return Fail(work, "output_physical", result);
        for (uint32_t i = 0; i < deviceCount && !physical; ++i) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &count, nullptr);
            if (!count || count > 32) continue;
            VkQueueFamilyProperties families[32]{};
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &count, families);
            for (uint32_t family = 0; family < count; ++family) {
                VkBool32 present = VK_FALSE;
                if (families[family].queueCount &&
                    (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], family, surface,
                                                         &present) == VK_SUCCESS && present) {
                    physical = devices[i];
                    queueFamily = family;
                    break;
                }
            }
        }
        if (!physical) return Fail(work, "output_queue", VK_ERROR_INITIALIZATION_FAILED);
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        result = vkCreateDevice(physical, &deviceInfo, nullptr, &device);
        if (result != VK_SUCCESS) return Fail(work, "output_device", result);
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (!queue) return Fail(work, "output_device_queue", VK_ERROR_INITIALIZATION_FAILED);

        VkSurfaceCapabilitiesKHR caps{};
        result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
        if (result != VK_SUCCESS) return Fail(work, "output_capabilities", result);
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return Fail(work, "output_transfer_usage", VK_ERROR_FORMAT_NOT_SUPPORTED);
        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
        if (result != VK_SUCCESS || !formatCount)
            return Fail(work, "output_format_count", VK_ERROR_FORMAT_NOT_SUPPORTED);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount,
                                                      formats.data());
        if (result != VK_SUCCESS) return Fail(work, "output_formats", result);
        VkSurfaceFormatKHR format = formats[0];
        for (const auto& candidate : formats) {
            if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM ||
                candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
                format = candidate;
                break;
            }
        }
        if (format.format != VK_FORMAT_R8G8B8A8_UNORM &&
            format.format != VK_FORMAT_B8G8R8A8_UNORM)
            return Fail(work, "output_rgba_format", VK_ERROR_FORMAT_NOT_SUPPORTED);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(320u, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(240u, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!extent.width || !extent.height)
            return Fail(work, "output_extent", VK_ERROR_INITIALIZATION_FAILED);
        uint32_t count = std::max(caps.minImageCount, 2u);
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha)) {
            const VkCompositeAlphaFlagBitsKHR choices[] = {
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
            for (auto choice : choices) {
                if (caps.supportedCompositeAlpha & choice) {
                    alpha = choice;
                    break;
                }
            }
        }
        VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapInfo.surface = surface;
        swapInfo.minImageCount = count;
        swapInfo.imageFormat = format.format;
        swapInfo.imageColorSpace = format.colorSpace;
        swapInfo.imageExtent = extent;
        swapInfo.imageArrayLayers = 1;
        swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapInfo.preTransform = caps.currentTransform;
        swapInfo.compositeAlpha = alpha;
        swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapInfo.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device, &swapInfo, nullptr, &swapchain);
        if (result != VK_SUCCESS) return Fail(work, "output_swapchain", result);
        uint32_t imageCount = 0;
        result = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        if (result != VK_SUCCESS || !imageCount)
            return Fail(work, "output_image_count", VK_ERROR_INITIALIZATION_FAILED);
        images.resize(imageCount);
        result = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
        if (result != VK_SUCCESS) return Fail(work, "output_images", result);
        work.width = extent.width;
        work.height = extent.height;
        work.imageCount = imageCount;
        work.format = format.format;

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        result = vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
        if (result != VK_SUCCESS) return Fail(work, "output_command_pool", result);
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = pool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &commandInfo, &command);
        if (result != VK_SUCCESS) return Fail(work, "output_command_buffer", result);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired);
        if (result != VK_SUCCESS) return Fail(work, "output_acquire_semaphore", result);
        rendered.resize(imageCount, VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : rendered) {
            result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS) return Fail(work, "output_render_semaphore", result);
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device, &fenceInfo, nullptr, &submitted);
        if (result != VK_SUCCESS) return Fail(work, "output_fence", result);
        return true;
    }

    bool PresentClear(OutputWork& work, uint32_t frame)
    {
        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(device, swapchain, 5'000'000'000ULL,
                                                acquired, VK_NULL_HANDLE, &imageIndex);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return Fail(work, "output_acquire", result);
        result = vkResetCommandBuffer(command, 0);
        if (result != VK_SUCCESS) return Fail(work, "output_command_reset", result);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &beginInfo);
        if (result != VK_SUCCESS) return Fail(work, "output_command_begin", result);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.image = images[imageIndex];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        VkClearColorValue color{};
        color.float32[0] = static_cast<float>(frame * 31 + 7) / 255.0f;
        color.float32[1] = 90.0f / 255.0f;
        color.float32[2] = 165.0f / 255.0f;
        color.float32[3] = 1.0f;
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(command, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &range);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) return Fail(work, "output_command_end", result);
        result = vkResetFences(device, 1, &submitted);
        if (result != VK_SUCCESS) return Fail(work, "output_fence_reset", result);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[imageIndex];
        result = vkQueueSubmit(queue, 1, &submit, submitted);
        if (result != VK_SUCCESS) return Fail(work, "output_submit", result);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(queue, &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return Fail(work, "output_present", result);
        result = vkWaitForFences(device, 1, &submitted, VK_TRUE, 5'000'000'000ULL);
        if (result != VK_SUCCESS) return Fail(work, "output_wait", result);
        ++work.frames;
        return true;
    }
};

void ExecuteOutput(napi_env, void* data)
{
    auto& work = *static_cast<OutputWork*>(data);
    uint64_t surfaceId = 0;
    {
        std::unique_lock<std::mutex> lock(g_surfaceMutex);
        if (!g_surfaceReady.wait_for(lock, std::chrono::seconds(10),
                                     [] { return g_surfaceId != 0; })) {
            work.stage = "output_surface_timeout";
            work.error = VK_TIMEOUT;
            return;
        }
        surfaceId = g_surfaceId;
    }
    work.surfaceId = surfaceId;
    OutputVulkan output;
    if (!output.Initialize(work, surfaceId)) return;
    for (uint32_t frame = 0; frame < 8; ++frame) {
        if (!output.PresentClear(work, frame)) return;
    }
    // Queue submission is not a display-completion signal. Keep the diagnostic
    // swapchain alive long enough for the system compositor to show the last
    // FIFO image before releasing the window and its Vulkan resources.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    work.stage = "complete";
}

void CompleteOutput(napi_env env, napi_status status, void* data)
{
    auto* work = static_cast<OutputWork*>(data);
    napi_value object;
    napi_create_object(env, &object);
    auto setString = [&](const char* key, const char* value) {
        napi_value item;
        napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &item);
        napi_set_named_property(env, object, key, item);
    };
    auto setInt = [&](const char* key, int32_t value) {
        napi_value item;
        napi_create_int32(env, value, &item);
        napi_set_named_property(env, object, key, item);
    };
    setString("gate", "D2-OUTPUT");
    setString("status", status == napi_ok && work->frames == 8 ? "PASS" : "FAIL");
    setString("stage", work->stage);
    setInt("vkResult", static_cast<int32_t>(work->error));
    setInt("framesPresented", static_cast<int32_t>(work->frames));
    setInt("width", static_cast<int32_t>(work->width));
    setInt("height", static_cast<int32_t>(work->height));
    setInt("imageCount", static_cast<int32_t>(work->imageCount));
    setInt("format", static_cast<int32_t>(work->format));
    char surfaceIdText[32]{};
    std::snprintf(surfaceIdText, sizeof(surfaceIdText), "%llu",
                  static_cast<unsigned long long>(work->surfaceId));
    setString("surfaceId", surfaceIdText);
    setInt("sequence", static_cast<int32_t>(work->sequence));
    setInt("pid", static_cast<int32_t>(getpid()));
    setInt("parentFdCount", CountOpenFds());
    setInt("parentRssKiB", ReadRssKiB());
    napi_resolve_deferred(env, work->deferred, object);
    napi_delete_async_work(env, work->asyncWork);
    delete work;
}

} // namespace

bool WaitDirectProbeSurfaceId(uint64_t* surfaceId, uint32_t timeoutMs)
{
    if (!surfaceId) return false;
    std::unique_lock<std::mutex> lock(g_surfaceMutex);
    if (!g_surfaceReady.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [] { return g_surfaceId != 0; })) return false;
    *surfaceId = g_surfaceId;
    return true;
}

bool RequestDirectProbeResize(uint64_t surfaceId, uint32_t oldWidth, uint32_t oldHeight,
                              uint32_t* newWidth, uint32_t* newHeight, uint32_t timeoutMs)
{
    if (!surfaceId || !newWidth || !newHeight) return false;
    std::unique_lock<std::mutex> lock(g_surfaceMutex);
    if (g_surfaceId != surfaceId) return false;
    const uint64_t previousGeneration = g_surfaceSizeGeneration;
    g_resizeRequested = true;
    if (!g_surfaceReady.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return g_surfaceId != surfaceId ||
                (g_surfaceSizeGeneration > previousGeneration && g_surfaceWidth && g_surfaceHeight &&
                 (g_surfaceWidth != oldWidth || g_surfaceHeight != oldHeight));
        })) {
        g_resizeRequested = false;
        return false;
    }
    if (g_surfaceId != surfaceId) return false;
    *newWidth = g_surfaceWidth;
    *newHeight = g_surfaceHeight;
    return true;
}

napi_value SetDirectProbeSurfaceId(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t value = 0;
    bool lossless = false;
    if (argc != 1 || napi_get_value_bigint_int64(env, args[0], &value, &lossless) != napi_ok ||
        !lossless || value < 0) {
        napi_throw_type_error(env, nullptr, "expected nonnegative surface id BigInt");
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_surfaceMutex);
        if (g_surfaceId != static_cast<uint64_t>(value)) {
            g_surfaceId = static_cast<uint64_t>(value);
            g_surfaceWidth = 0;
            g_surfaceHeight = 0;
            g_resizeRequested = false;
            ++g_surfaceSizeGeneration;
        }
    }
    g_surfaceReady.notify_all();
    return nullptr;
}

napi_value ClearDirectProbeSurfaceId(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t value = 0;
    bool lossless = false;
    if (argc != 1 || napi_get_value_bigint_int64(env, args[0], &value, &lossless) != napi_ok ||
        !lossless || value <= 0) {
        napi_throw_type_error(env, nullptr, "expected positive surface id BigInt");
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    if (g_surfaceId == static_cast<uint64_t>(value)) {
        g_surfaceId = 0;
        g_surfaceWidth = 0;
        g_surfaceHeight = 0;
        g_resizeRequested = false;
        ++g_surfaceSizeGeneration;
        g_surfaceReady.notify_all();
    }
    return nullptr;
}

napi_value SetDirectProbeSurfaceSize(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t surfaceId = 0;
    bool lossless = false;
    uint32_t width = 0;
    uint32_t height = 0;
    if (argc != 3 ||
        napi_get_value_bigint_int64(env, args[0], &surfaceId, &lossless) != napi_ok ||
        !lossless || surfaceId <= 0 ||
        napi_get_value_uint32(env, args[1], &width) != napi_ok ||
        napi_get_value_uint32(env, args[2], &height) != napi_ok ||
        !width || !height) {
        napi_throw_type_error(env, nullptr, "expected surface id and positive size");
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_surfaceMutex);
        if (g_surfaceId == static_cast<uint64_t>(surfaceId)) {
            g_surfaceWidth = width;
            g_surfaceHeight = height;
            ++g_surfaceSizeGeneration;
        }
    }
    g_surfaceReady.notify_all();
    return nullptr;
}

napi_value TakeDirectProbeResizeRequest(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t surfaceId = 0;
    bool lossless = false;
    if (argc != 1 ||
        napi_get_value_bigint_int64(env, args[0], &surfaceId, &lossless) != napi_ok ||
        !lossless || surfaceId < 0) {
        napi_throw_type_error(env, nullptr, "expected surface id BigInt");
        return nullptr;
    }
    bool requested;
    {
        std::lock_guard<std::mutex> lock(g_surfaceMutex);
        requested = g_resizeRequested && g_surfaceId == static_cast<uint64_t>(surfaceId);
        if (requested) g_resizeRequested = false;
    }
    napi_value result;
    napi_get_boolean(env, requested, &result);
    return result;
}

napi_value RunDirectOutputProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) OutputWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate output probe");
        return nullptr;
    }
    work->sequence = g_runSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        napi_throw_error(env, nullptr, "failed to create output probe promise");
        return nullptr;
    }
    napi_value resourceName;
    napi_create_string_utf8(env, "WineHuaDirectD2Output", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_async_work(env, nullptr, resourceName, ExecuteOutput,
                               CompleteOutput, work, &work->asyncWork) != napi_ok ||
        napi_queue_async_work(env, work->asyncWork) != napi_ok) {
        if (work->asyncWork) napi_delete_async_work(env, work->asyncWork);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue output probe");
        return nullptr;
    }
    return promise;
}

} // namespace winehua::direct
