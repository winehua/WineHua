#define VK_USE_PLATFORM_OHOS 1

#include "graphics/native_window_vk_target.h"

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "venus-direct"

namespace winehua {
namespace {

using SteadyClock = std::chrono::steady_clock;

uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

void CloseFd(int* fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

uint32_t PickMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if (typeBits & (1u << i)) return i;
    }
    return UINT32_MAX;
}

int32_t NativePixelFormat(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    default:
        return NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
    }
}

} // namespace

const char* NativeWindowVkConfigureResultName(NativeWindowVkConfigureResult result)
{
    switch (result) {
    case NativeWindowVkConfigureResult::Ready:
        return "ready";
    case NativeWindowVkConfigureResult::InvalidOwner:
        return "invalid-owner";
    case NativeWindowVkConfigureResult::MissingExtension:
        return "missing-ohos-native-buffer-extension";
    case NativeWindowVkConfigureResult::WindowConfigurationFailed:
        return "window-configuration-failed";
    }
    return "unknown";
}

const char* NativeWindowVkBeginResultName(NativeWindowVkBeginResult result)
{
    switch (result) {
    case NativeWindowVkBeginResult::Ready:
        return "ready";
    case NativeWindowVkBeginResult::Deferred:
        return "queue-full";
    case NativeWindowVkBeginResult::ImportFailed:
        return "native-buffer-import-failed";
    }
    return "unknown";
}

NativeWindowVkConfigureResult NativeWindowVkTarget::Configure(
    uint64_t surfaceKey,
    OHNativeWindow* window,
    uint32_t width,
    uint32_t height,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkFormat sourceFormat)
{
    if (!surfaceKey || !window || !width || !height || !physicalDevice || !device)
        return NativeWindowVkConfigureResult::InvalidOwner;

    const int32_t nativeFormat = NativePixelFormat(sourceFormat);
    if (Ready()) {
        const bool sameOwner = surfaceKey_ == surfaceKey && window_ == window &&
            width_ == width && height_ == height && nativeFormat_ == nativeFormat &&
            physicalDevice_ == physicalDevice && device_ == device;
        return sameOwner ? NativeWindowVkConfigureResult::Ready
                         : NativeWindowVkConfigureResult::InvalidOwner;
    }

    surfaceKey_ = surfaceKey;
    window_ = window;
    width_ = width;
    height_ = height;
    nativeFormat_ = nativeFormat;
    physicalDevice_ = physicalDevice;
    device_ = device;
    getProperties_ = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(
        vkGetDeviceProcAddr(device_, "vkGetNativeBufferPropertiesOHOS"));
    acquireImage_ = reinterpret_cast<PFN_vkAcquireImageOHOS>(
        vkGetDeviceProcAddr(device_, "vkAcquireImageOHOS"));
    signalReleaseImage_ = reinterpret_cast<PFN_vkQueueSignalReleaseImageOHOS>(
        vkGetDeviceProcAddr(device_, "vkQueueSignalReleaseImageOHOS"));
    if (!getProperties_ || !acquireImage_ || !signalReleaseImage_) {
        Abandon();
        return NativeWindowVkConfigureResult::MissingExtension;
    }

    const int32_t geometryResult = OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_BUFFER_GEOMETRY, static_cast<int32_t>(width_),
        static_cast<int32_t>(height_));
    const int32_t usageResult = OH_NativeWindow_NativeWindowHandleOpt(
        window_, SET_USAGE,
        static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER |
                              NATIVEBUFFER_USAGE_HW_TEXTURE));
    const int32_t formatResult =
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, nativeFormat_);
    if (geometryResult || usageResult || formatResult) {
        OH_LOG_WARN(LOG_APP,
                    "[VENUS-DIRECT] capability probe failed key=%{public}llu "
                    "geometry=%{public}d usage=%{public}d format=%{public}d",
                    static_cast<unsigned long long>(surfaceKey_), geometryResult,
                    usageResult, formatResult);
        Abandon();
        return NativeWindowVkConfigureResult::WindowConfigurationFailed;
    }

    appliedTimeoutMs_ = INT32_MIN;
    if (!SetRequestTimeoutMs(100)) {
        Abandon();
        return NativeWindowVkConfigureResult::WindowConfigurationFailed;
    }
    OH_LOG_INFO(LOG_APP,
                "[VENUS-DIRECT] capability probe ready key=%{public}llu "
                "size=%{public}ux%{public}u source_format=%{public}u "
                "native_format=%{public}d",
                static_cast<unsigned long long>(surfaceKey_), width_, height_,
                static_cast<uint32_t>(sourceFormat), nativeFormat_);
    return NativeWindowVkConfigureResult::Ready;
}

bool NativeWindowVkTarget::SetRequestTimeoutMs(int32_t timeoutMs)
{
    if (!window_) return false;
    if (appliedTimeoutMs_ == timeoutMs) return true;
    const int32_t result =
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, timeoutMs);
    if (result) {
        OH_LOG_ERROR(LOG_APP,
                     "[VENUS-DIRECT] SET_TIMEOUT failed key=%{public}llu "
                     "timeout_ms=%{public}d result=%{public}d",
                     static_cast<unsigned long long>(surfaceKey_), timeoutMs,
                     result);
        return false;
    }
    appliedTimeoutMs_ = timeoutMs;
    return true;
}

bool NativeWindowVkTarget::MatchesOwner(const Slot& slot, uint32_t seq) const
{
    return slot.surfaceKey == surfaceKey_ && slot.seq == seq &&
        slot.width == width_ && slot.height == height_ &&
        slot.nativeFormat == nativeFormat_ &&
        slot.physicalDevice == physicalDevice_ && slot.device == device_;
}

NativeWindowVkTarget::Slot* NativeWindowVkTarget::Import(
    OHNativeWindowBuffer* windowBuffer)
{
    OH_NativeBuffer* nativeBuffer = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &nativeBuffer) != 0 ||
        !nativeBuffer) {
        return nullptr;
    }
    const uint32_t seq = OH_NativeBuffer_GetSeqNum(nativeBuffer);
    for (Slot& slot : slots_) {
        if (MatchesOwner(slot, seq)) {
            slot.windowBuffer = windowBuffer;
            slot.nativeBuffer = nativeBuffer;
            return &slot;
        }
    }
    if (slots_.size() >= kMaxImportedSlotsPerAttach) {
        OH_LOG_ERROR(LOG_APP,
                     "[VENUS-DIRECT] import cache limit key=%{public}llu "
                     "slots=%{public}zu seq=%{public}u",
                     static_cast<unsigned long long>(surfaceKey_), slots_.size(), seq);
        return nullptr;
    }

    VkNativeBufferFormatPropertiesOHOS formatProperties{
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS properties{
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    properties.pNext = &formatProperties;
    if (getProperties_(device_, nativeBuffer, &properties) != VK_SUCCESS)
        return nullptr;
    const VkFormat format = formatProperties.format != VK_FORMAT_UNDEFINED
        ? formatProperties.format
        : VK_FORMAT_R8G8B8A8_UNORM;

    VkExternalMemoryImageCreateInfo externalMemory{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalMemory.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalMemory;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage importedImage = VK_NULL_HANDLE;
    if (vkCreateImage(device_, &imageInfo, nullptr, &importedImage) != VK_SUCCESS)
        return nullptr;

    VkImportNativeBufferInfoOHOS importInfo{
        VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
    importInfo.buffer = nativeBuffer;
    VkMemoryDedicatedAllocateInfo dedicatedInfo{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = importedImage;
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.pNext = &dedicatedInfo;
    allocateInfo.allocationSize = properties.allocationSize;
    allocateInfo.memoryTypeIndex =
        PickMemoryType(physicalDevice_, properties.memoryTypeBits);

    VkDeviceMemory importedMemory = VK_NULL_HANDLE;
    if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device_, &allocateInfo, nullptr, &importedMemory) !=
            VK_SUCCESS) {
        vkDestroyImage(device_, importedImage, nullptr);
        return nullptr;
    }
    if (vkBindImageMemory(device_, importedImage, importedMemory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, importedMemory, nullptr);
        vkDestroyImage(device_, importedImage, nullptr);
        return nullptr;
    }

    slots_.push_back(Slot{
        surfaceKey_, seq, width_, height_, nativeFormat_, physicalDevice_, device_,
        windowBuffer, nativeBuffer, importedImage, importedMemory, format});
    OH_LOG_INFO(LOG_APP,
                "[VENUS-DIRECT] imported key=%{public}llu seq=%{public}u "
                "slots=%{public}zu format=%{public}u",
                static_cast<unsigned long long>(surfaceKey_), seq, slots_.size(),
                static_cast<uint32_t>(format));
    return &slots_.back();
}

NativeWindowVkBeginResult NativeWindowVkTarget::BeginFrame()
{
    if (!Ready() || current_) return NativeWindowVkBeginResult::ImportFailed;
    OHNativeWindowBuffer* windowBuffer = nullptr;
    int acquireFenceFd = -1;
    const uint64_t startedUs = NowUs();
    const int32_t result = OH_NativeWindow_NativeWindowRequestBuffer(
        window_, &windowBuffer, &acquireFenceFd);
    lastRequestUs_ = NowUs() - startedUs;
    if (result != 0 || !windowBuffer) {
        CloseFd(&acquireFenceFd);
        return NativeWindowVkBeginResult::Deferred;
    }

    current_ = Import(windowBuffer);
    if (!current_) {
        OH_NativeWindow_NativeWindowAbortBuffer(window_, windowBuffer);
        CloseFd(&acquireFenceFd);
        return NativeWindowVkBeginResult::ImportFailed;
    }
    pendingAcquireFd_ = acquireFenceFd;
    return NativeWindowVkBeginResult::Ready;
}

VkResult NativeWindowVkTarget::AcquireGpu(VkSemaphore semaphore, VkFence fence)
{
    if (!current_ || !acquireImage_) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result =
        acquireImage_(device_, current_->image, pendingAcquireFd_, semaphore, fence);
    // vkAcquireImageOHOS consumes the fd on every Vulkan return path.
    pendingAcquireFd_ = -1;
    if (result != VK_SUCCESS) AbortFrame();
    return result;
}

VkResult NativeWindowVkTarget::SignalRelease(
    VkQueue queue,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* waitSemaphores,
    int* releaseFenceFd)
{
    if (releaseFenceFd) *releaseFenceFd = -1;
    if (!current_ || !signalReleaseImage_ || !releaseFenceFd)
        return VK_ERROR_INITIALIZATION_FAILED;
    return signalReleaseImage_(queue, waitSemaphoreCount, waitSemaphores,
                               current_->image, releaseFenceFd);
}

int32_t NativeWindowVkTarget::EndFrame(int releaseFenceFd, uint64_t timestampNs)
{
    if (!window_ || !current_ || !current_->windowBuffer) {
        CloseFd(&releaseFenceFd);
        return -1;
    }
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_UI_TIMESTAMP, timestampNs);
    Region region{};
    region.rects = nullptr;
    region.rectNumber = 0;
    const uint64_t startedUs = NowUs();
    const int32_t result = OH_NativeWindow_NativeWindowFlushBuffer(
        window_, current_->windowBuffer, releaseFenceFd, region);
    lastFlushUs_ = NowUs() - startedUs;
    if (result != 0) {
        CloseFd(&releaseFenceFd);
        // Caller must retire GPU work before aborting a failed flush.
        // Retain current_ so Reset/AbortFrame can return this buffer safely.
    } else {
        current_ = nullptr;
    }
    return result;
}

void NativeWindowVkTarget::AbortFrame()
{
    CloseFd(&pendingAcquireFd_);
    if (window_ && current_ && current_->windowBuffer)
        OH_NativeWindow_NativeWindowAbortBuffer(window_, current_->windowBuffer);
    current_ = nullptr;
}

void NativeWindowVkTarget::DestroySlot(Slot& slot)
{
    if (device_ && slot.image) vkDestroyImage(device_, slot.image, nullptr);
    if (device_ && slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
    slot = Slot{};
}

void NativeWindowVkTarget::Reset()
{
    AbortFrame();
    for (Slot& slot : slots_) DestroySlot(slot);
    slots_.clear();
    Abandon();
}

void NativeWindowVkTarget::Abandon()
{
    // Do not issue Vulkan destruction here: callers use this only when the
    // device owner can no longer prove that the VkDevice is alive and idle.
    CloseFd(&pendingAcquireFd_);
    current_ = nullptr;
    slots_.clear();
    surfaceKey_ = 0;
    window_ = nullptr;
    width_ = 0;
    height_ = 0;
    nativeFormat_ = 0;
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    getProperties_ = nullptr;
    acquireImage_ = nullptr;
    signalReleaseImage_ = nullptr;
    appliedTimeoutMs_ = INT32_MIN;
}

VkImage NativeWindowVkTarget::ColorImage() const
{
    return current_ ? current_->image : VK_NULL_HANDLE;
}

VkFormat NativeWindowVkTarget::ColorFormat() const
{
    return current_ ? current_->format : VK_FORMAT_UNDEFINED;
}

uint32_t NativeWindowVkTarget::CurrentSeq() const
{
    return current_ ? current_->seq : 0;
}

} // namespace winehua
