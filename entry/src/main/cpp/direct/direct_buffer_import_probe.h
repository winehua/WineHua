#pragma once

#include "direct_vulkan_context.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct OH_NativeBuffer;

namespace winehua::direct {

// Imports BufferQueue images and exercises GPU acquire/release SYNC_FD handoff.
// Reserved for diagnostic callers. Live Wine frames use DirectVulkanPresenter.
class DirectBufferImportProbe {
public:
    explicit DirectBufferImportProbe(bool enableFences = false, uint64_t outputSurfaceId = 0,
                                     uint32_t frameSlots = 1, bool verifyPixels = true,
                                     bool externalSharedImage = false,
                                     bool identityOutput = false)
        : context_({outputSurfaceId, frameSlots, enableFences, identityOutput}),
          fenceMode_(enableFences), outputSurfaceId_(outputSurfaceId),
          frameSlotCount_(frameSlots), verifyPixels_(verifyPixels),
          externalSharedImage_(externalSharedImage) {}
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
    uint32_t OutputWidth() const { return context_.OutputExtent().width; }
    uint32_t OutputHeight() const { return context_.OutputExtent().height; }
    uint32_t OutputRecreateCount() const { return context_.OutputRecreateCount(); }

private:
    struct FrameSlot : DirectVulkanSubmission {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
        int32_t frame = -1;
        bool inFlight = false;
        std::shared_ptr<DirectVulkanImage> imageReference;
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

    DirectVulkanContext context_;
    bool fenceMode_ = false;
    uint64_t outputSurfaceId_ = 0;
    uint32_t frameSlotCount_ = 1;
    bool verifyPixels_ = true;
    bool externalSharedImage_ = false;
    std::vector<FrameSlot> frameSlots_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline outputPipeline_ = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, std::shared_ptr<DirectVulkanImage>> cache_;
    VkResult error_ = VK_SUCCESS;
    const char* stage_ = "pending";
    uint32_t imports_ = 0;
    uint32_t reuses_ = 0;
    uint32_t samples_ = 0;
    uint32_t acquireImports_ = 0;
    uint32_t releaseExports_ = 0;
    uint32_t outputPresents_ = 0;
};

} // namespace winehua::direct
