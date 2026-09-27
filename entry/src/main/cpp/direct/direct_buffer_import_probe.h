#pragma once

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <native_window/external_window.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct OH_NativeBuffer;

namespace winehua::direct {

// Diagnostic only: imports BufferQueue images, samples them on the GPU and
// exercises acquire/release SYNC_FD handoff. Production composition is separate.
class DirectBufferImportProbe {
public:
    explicit DirectBufferImportProbe(bool enableFences = false, uint64_t outputSurfaceId = 0,
                                     uint32_t frameSlots = 1)
        : fenceMode_(enableFences), outputSurfaceId_(outputSurfaceId),
          frameSlotCount_(frameSlots) {}
    ~DirectBufferImportProbe();
    bool Import(OH_NativeBuffer* buffer, int32_t width, int32_t height);
    bool Sample(OH_NativeBuffer* buffer, int32_t width, int32_t height, int32_t frame);
    bool SubmitSampleWithFences(OH_NativeBuffer* buffer, int32_t width, int32_t height,
                                int32_t frame, int* acquireFence, int* releaseFence);
    bool FinishSample(int32_t frame);
    void NewGeneration();
    bool RecreateOutput();
    int32_t VkError() const { return static_cast<int32_t>(error_); }
    const char* Stage() const { return stage_; }
    uint32_t ImportCount() const { return imports_; }
    uint32_t ReuseCount() const { return reuses_; }
    uint32_t SampleCount() const { return samples_; }
    uint32_t AcquireImportCount() const { return acquireImports_; }
    uint32_t ReleaseExportCount() const { return releaseExports_; }
    uint32_t OutputPresentCount() const { return outputPresents_; }
    uint32_t OutputWidth() const { return outputExtent_.width; }
    uint32_t OutputHeight() const { return outputExtent_.height; }
    uint32_t OutputRecreateCount() const { return outputRecreates_; }

private:
    struct Entry {
        OH_NativeBuffer* buffer = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct FrameSlot {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
        VkSemaphore releaseSemaphore = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
        int32_t frame = -1;
        bool inFlight = false;
    };
    bool Initialize();
    bool InitializeSampler();
    bool InitializeSync();
    bool InitializeOutput();
    void DestroyOutput();
    bool RecordAndSubmit(OH_NativeBuffer* buffer, int32_t width, int32_t height, int32_t frame,
                         int* acquireFence, int* releaseFence);
    bool Fail(const char* stage, VkResult result);
    void ClearCache();

    VkInstance instance_ = VK_NULL_HANDLE;
    bool fenceMode_ = false;
    uint64_t outputSurfaceId_ = 0;
    uint32_t frameSlotCount_ = 1;
    OHNativeWindow* outputWindow_ = nullptr;
    VkSurfaceKHR outputSurface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<FrameSlot> frameSlots_;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd_ = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSwapchainKHR outputSwapchain_ = VK_NULL_HANDLE;
    VkExtent2D outputExtent_{};
    VkFormat outputFormat_ = VK_FORMAT_UNDEFINED;
    VkRenderPass outputRenderPass_ = VK_NULL_HANDLE;
    VkPipeline outputPipeline_ = VK_NULL_HANDLE;
    std::vector<VkImage> outputImages_;
    std::vector<VkImageView> outputViews_;
    std::vector<VkFramebuffer> outputFramebuffers_;
    std::vector<VkSemaphore> outputAcquired_;
    std::vector<VkSemaphore> outputRendered_;
    std::unordered_map<uint32_t, Entry> cache_;
    VkResult error_ = VK_SUCCESS;
    const char* stage_ = "pending";
    uint32_t imports_ = 0;
    uint32_t reuses_ = 0;
    uint32_t samples_ = 0;
    uint32_t acquireImports_ = 0;
    uint32_t releaseExports_ = 0;
    uint32_t outputPresents_ = 0;
    uint32_t outputRecreates_ = 0;
};

} // namespace winehua::direct
