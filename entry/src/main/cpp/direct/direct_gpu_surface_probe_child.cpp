#define VK_USE_PLATFORM_OHOS 1

#include "surface_probe_protocol.h"

#include <IPCKit/ipc_kit.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <vulkan/vulkan.h>

#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_D2_WSI_CHILD"
#include <hilog/log.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

using winehua::direct::SurfaceProbeFrame;

struct GpuProducer {
    OHNativeWindow* window = nullptr; // Owned by the IPC handler, not by Vulkan.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence submitted = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> presented;
    std::vector<bool> initialized;
    uint32_t queueFamily = UINT32_MAX;
    int32_t width = 0;
    int32_t height = 0;

    ~GpuProducer() { Reset(); }

    void DestroySwapchain()
    {
        if (!device) return;
        if (swapchain) vkQueueWaitIdle(queue);
        for (VkSemaphore semaphore : presented) {
            if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        }
        presented.clear();
        initialized.clear();
        images.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    void Reset()
    {
        DestroySwapchain();
        if (device) {
            if (submitted) vkDestroyFence(device, submitted, nullptr);
            if (acquired) vkDestroySemaphore(device, acquired, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface && instance) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        physical = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        commandPool = VK_NULL_HANDLE;
        command = VK_NULL_HANDLE;
        acquired = VK_NULL_HANDLE;
        submitted = VK_NULL_HANDLE;
        queueFamily = UINT32_MAX;
        width = height = 0;
        window = nullptr;
    }

    VkResult Initialize(OHNativeWindow* producer, int32_t w, int32_t h,
                        SurfaceProbeFrame& output)
    {
        window = producer;
        output.stage = winehua::direct::kGpuInstance;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WineHua Direct D2 WSI";
        app.apiVersion = VK_API_VERSION_1_1;
        const char* instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
        if (result != VK_SUCCESS) return result;

        output.stage = winehua::direct::kGpuSurface;
        VkSurfaceCreateInfoOHOS surfaceInfo{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
        surfaceInfo.window = window;
        result = vkCreateSurfaceOHOS(instance, &surfaceInfo, nullptr, &surface);
        if (result != VK_SUCCESS) return result;
        uint32_t physicalCount = 0;
        result = vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
        if (result != VK_SUCCESS || !physicalCount || physicalCount > 16)
            return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
        VkPhysicalDevice devices[16]{};
        result = vkEnumeratePhysicalDevices(instance, &physicalCount, devices);
        if (result != VK_SUCCESS) return result;
        for (uint32_t i = 0; i < physicalCount && !physical; ++i) {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, nullptr);
            if (!familyCount || familyCount > 32) continue;
            VkQueueFamilyProperties families[32]{};
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, families);
            for (uint32_t family = 0; family < familyCount; ++family) {
                VkBool32 presentSupport = VK_FALSE;
                if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                    vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], family, surface,
                                                         &presentSupport) != VK_SUCCESS ||
                    !presentSupport) continue;
                physical = devices[i];
                queueFamily = family;
                break;
            }
        }
        if (!physical) return VK_ERROR_INITIALIZATION_FAILED;

        output.stage = winehua::direct::kGpuDevice;
        const float priority = 1.0f;
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
        if (result != VK_SUCCESS) return result;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (result != VK_SUCCESS) return result;
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &commandInfo, &command);
        if (result != VK_SUCCESS) return result;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired);
        if (result != VK_SUCCESS) return result;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device, &fenceInfo, nullptr, &submitted);
        if (result != VK_SUCCESS) return result;
        return CreateSwapchain(w, h, output);
    }

    VkResult CreateSwapchain(int32_t w, int32_t h, SurfaceProbeFrame& output)
    {
        output.stage = winehua::direct::kGpuSwapchain;
        DestroySwapchain(); // Resize only: never destroy buffers while the queue uses them.
        if (OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, w, h) != 0 ||
            OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT,
                                                  NATIVEBUFFER_PIXEL_FMT_RGBA_8888) != 0 ||
            OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE,
                static_cast<uint64_t>(NATIVEBUFFER_USAGE_CPU_READ |
                                      NATIVEBUFFER_USAGE_HW_RENDER |
                                      NATIVEBUFFER_USAGE_HW_TEXTURE)) != 0)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkSurfaceCapabilitiesKHR caps{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
        if (result != VK_SUCCESS) return result;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
        if (result != VK_SUCCESS || !formatCount) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount,
                                                      formats.data());
        if (result != VK_SUCCESS) return result;
        VkSurfaceFormatKHR format{};
        bool found = false;
        for (const auto& candidate : formats) {
            if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM) {
                format = candidate;
                found = true;
                break;
            }
        }
        if (!found) return VK_ERROR_FORMAT_NOT_SUPPORTED;

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width,
                                      caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height,
                                       caps.maxImageExtent.height);
        }
        if (extent.width != static_cast<uint32_t>(w) ||
            extent.height != static_cast<uint32_t>(h))
            return VK_ERROR_INITIALIZATION_FAILED;
        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface;
        create.minImageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount) create.minImageCount = std::min(create.minImageCount, caps.maxImageCount);
        create.imageFormat = format.format;
        create.imageColorSpace = format.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = caps.currentTransform;
        create.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha &
                (~caps.supportedCompositeAlpha + 1));
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device, &create, nullptr, &swapchain);
        if (result != VK_SUCCESS) return result;
        uint32_t imageCount = 0;
        result = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        if (result != VK_SUCCESS || !imageCount) return VK_ERROR_INITIALIZATION_FAILED;
        images.resize(imageCount);
        result = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
        if (result != VK_SUCCESS) return result;
        initialized.assign(imageCount, false);
        presented.assign(imageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& semaphore : presented) {
            result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS) return result;
        }
        width = w;
        height = h;
        return VK_SUCCESS;
    }

    VkResult Render(int32_t frame, int32_t w, int32_t h, SurfaceProbeFrame& output)
    {
        if (!instance) {
            VkResult result = Initialize(window, w, h, output);
            if (result != VK_SUCCESS) return result;
        } else if (width != w || height != h) {
            VkResult result = CreateSwapchain(w, h, output);
            if (result != VK_SUCCESS) return result;
        }
        output.stage = winehua::direct::kGpuAcquire;
        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(device, swapchain, 5'000'000'000ULL,
                                                 acquired, VK_NULL_HANDLE, &imageIndex);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
        if (imageIndex >= images.size()) return VK_ERROR_INITIALIZATION_FAILED;
        output.bufferSeq = imageIndex; // Swapchain image index, not NativeBuffer sequence.

        output.stage = winehua::direct::kGpuRecord;
        result = vkResetCommandPool(device, commandPool, 0);
        if (result != VK_SUCCESS) return result;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) return result;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = initialized[imageIndex]
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[imageIndex];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        VkClearColorValue color{};
        color.float32[0] = static_cast<float>((frame % 8) * 31 + 7) / 255.0f;
        color.float32[1] = 90.0f / 255.0f;
        color.float32[2] = 165.0f / 255.0f;
        color.float32[3] = 1.0f;
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(command, images[imageIndex],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) return result;

        output.stage = winehua::direct::kGpuSubmit;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &presented[imageIndex];
        result = vkResetFences(device, 1, &submitted);
        if (result != VK_SUCCESS) return result;
        result = vkQueueSubmit(queue, 1, &submit, submitted);
        if (result != VK_SUCCESS) return result;
        result = vkWaitForFences(device, 1, &submitted, VK_TRUE, 5'000'000'000ULL);
        if (result != VK_SUCCESS) return result;
        initialized[imageIndex] = true;

        output.stage = winehua::direct::kGpuPresent;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &presented[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(queue, &present);
        return result == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : result;
    }
};

std::mutex g_mutex;
std::condition_variable g_condition;
bool g_finished = false;
OHNativeWindow* g_window = nullptr;
GpuProducer g_gpu;

void ReleaseLocked()
{
    g_gpu.Reset();
    if (g_window) OH_NativeWindow_DestroyNativeWindow(g_window);
    g_window = nullptr;
}

int OnRequest(uint32_t code, const OHIPCParcel* request, OHIPCParcel* reply, void*)
{
    int32_t version = 0;
    if (!request || !reply ||
        OH_IPCParcel_ReadInt32(request, &version) != OH_IPC_SUCCESS ||
        version != winehua::direct::kSurfaceProbeVersion)
        return OH_IPC_CHECK_PARAM_ERROR;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (code == winehua::direct::kSurfaceProbeFinish) {
        ReleaseLocked();
        g_finished = true;
        g_condition.notify_all();
        return OH_IPCParcel_WriteInt32(reply, 0);
    }
    if (code != winehua::direct::kSurfaceProbeProduce || g_finished)
        return OH_IPC_CHECK_PARAM_ERROR;
    int32_t frame = -1;
    int32_t width = 0;
    int32_t height = 0;
    if (OH_IPCParcel_ReadInt32(request, &frame) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &width) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &height) != OH_IPC_SUCCESS ||
        frame < 0 || frame >= winehua::direct::kGpuThroughputProbeFrameCount ||
        width <= 0 || height <= 0)
        return OH_IPC_CHECK_PARAM_ERROR;
    SurfaceProbeFrame output{};
    output.pid = getpid();
    output.frame = frame;
    output.width = width;
    output.height = height;
    output.stage = winehua::direct::kSurfaceReadWindow;
    if (frame == 0 && !g_window)
        OH_NativeWindow_ReadFromParcel(const_cast<OHIPCParcel*>(request), &g_window);
    if (g_window) {
        g_gpu.window = g_window;
        const VkResult result = g_gpu.Render(frame, width, height, output);
        output.status = static_cast<int32_t>(result);
        if (result == VK_SUCCESS) output.stage = winehua::direct::kSurfaceComplete;
    }
    OH_LOG_INFO(LOG_APP,
        "[DIRECT-D2-WSI] frame=%{public}d status=%{public}d stage=%{public}d pid=%{public}d "
        "size=%{public}dx%{public}d swapImage=%{public}u",
        frame, output.status, output.stage, output.pid, width, height, output.bufferSeq);
    return OH_IPCParcel_WriteBuffer(reply,
        reinterpret_cast<const uint8_t*>(&output), sizeof(output));
}

} // namespace

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    return OH_IPCRemoteStub_Create("winehua.direct.D2.WSI", OnRequest, nullptr, nullptr);
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_condition.wait_for(lock, std::chrono::seconds(120), [] { return g_finished; });
    ReleaseLocked();
}
