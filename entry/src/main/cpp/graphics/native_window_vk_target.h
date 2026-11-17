#pragma once

#include <native_window/external_window.h>

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <cstdint>
#include <vector>

struct OH_NativeBuffer;

namespace winehua {

enum class NativeWindowVkConfigureResult {
    Ready,
    InvalidOwner,
    MissingExtension,
    WindowConfigurationFailed,
};

enum class NativeWindowVkBeginResult {
    Ready,
    Deferred,
    ImportFailed,
};

const char* NativeWindowVkConfigureResultName(NativeWindowVkConfigureResult result);
const char* NativeWindowVkBeginResultName(NativeWindowVkBeginResult result);

/*
 * Owns only the Vulkan imports which back buffers dequeued from one
 * OHNativeWindow generation. The NativeWindow itself remains owned by the
 * presenter's NativeWindowLease.
 *
 * Slots are retained for the complete attach/device lifetime. In particular,
 * this class never evicts an imported image merely because another buffer was
 * dequeued: SurfaceQueue consumers may still retain older NativeBuffers.
 */
class NativeWindowVkTarget {
public:
    NativeWindowVkTarget() = default;
    ~NativeWindowVkTarget() = default;

    NativeWindowVkTarget(const NativeWindowVkTarget&) = delete;
    NativeWindowVkTarget& operator=(const NativeWindowVkTarget&) = delete;

    NativeWindowVkConfigureResult Configure(
        uint64_t surfaceKey,
        OHNativeWindow* window,
        uint32_t width,
        uint32_t height,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkFormat sourceFormat);
    bool SetRequestTimeoutMs(int32_t timeoutMs);
    NativeWindowVkBeginResult BeginFrame();
    VkResult AcquireGpu(VkSemaphore semaphore, VkFence fence = VK_NULL_HANDLE);
    VkResult SignalRelease(VkQueue queue,
                           uint32_t waitSemaphoreCount,
                           const VkSemaphore* waitSemaphores,
                           int* releaseFenceFd);
    int32_t EndFrame(int releaseFenceFd, uint64_t timestampNs);
    void AbortFrame();

    // Reset is valid only while the owning VkDevice is alive and idle.
    void Reset();
    // Used when the owner disappeared or cannot prove successful GPU retirement.
    void Abandon();

    bool Ready() const { return window_ && device_; }
    bool HasCurrent() const { return current_ != nullptr; }
    VkImage ColorImage() const;
    VkFormat ColorFormat() const;
    uint32_t CurrentSeq() const;
    uint64_t LastRequestUs() const { return lastRequestUs_; }
    uint64_t LastFlushUs() const { return lastFlushUs_; }
    size_t ImportedSlotCount() const { return slots_.size(); }

private:
    struct Slot {
        uint64_t surfaceKey = 0;
        uint32_t seq = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        int32_t nativeFormat = 0;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        OHNativeWindowBuffer* windowBuffer = nullptr;
        OH_NativeBuffer* nativeBuffer = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };

    Slot* Import(OHNativeWindowBuffer* windowBuffer);
    void DestroySlot(Slot& slot);
    bool MatchesOwner(const Slot& slot, uint32_t seq) const;

    static constexpr size_t kMaxImportedSlotsPerAttach = 64;

    uint64_t surfaceKey_ = 0;
    OHNativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int32_t nativeFormat_ = 0;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkGetNativeBufferPropertiesOHOS getProperties_ = nullptr;
    PFN_vkAcquireImageOHOS acquireImage_ = nullptr;
    PFN_vkQueueSignalReleaseImageOHOS signalReleaseImage_ = nullptr;
    std::vector<Slot> slots_;
    Slot* current_ = nullptr;
    int pendingAcquireFd_ = -1;
    int32_t appliedTimeoutMs_ = INT32_MIN;
    uint64_t lastRequestUs_ = 0;
    uint64_t lastFlushUs_ = 0;
};

} // namespace winehua
