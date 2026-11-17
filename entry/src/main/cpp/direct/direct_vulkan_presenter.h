#pragma once

#include "direct_vulkan_context.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace winehua::direct {

// Production single-image consumer. It samples an imported BufferQueue image
// directly into an OHOS output; there are no pixel mapping/readback resources.
// A zero output surface only retires the input queue until an output is bound.
// All methods, including destruction, belong to the controller worker thread.
class DirectVulkanPresenter {
public:
    explicit DirectVulkanPresenter(uint64_t outputSurfaceId, uint32_t frameSlots = 2)
        : context_({outputSurfaceId, frameSlots, true, true}),
          outputSurfaceId_(outputSurfaceId), frameSlotCount_(frameSlots) {}
    ~DirectVulkanPresenter();

    // The caller owns both fds. Successful submission consumes *acquireFence;
    // *releaseFence is transferred to BufferQueue even on a later present error.
    // On failure, destroy the presenter before returning the input buffer. An
    // unconsumed acquire fd can be returned to the queue as its release fence.
    bool Present(OH_NativeBuffer* buffer, int32_t width, int32_t height,
                 int* acquireFence, int* releaseFence);
    void InvalidateOutput() { recreatePending_ = outputSurfaceId_ != 0; }
    int32_t VkError() const { return static_cast<int32_t>(error_); }
    const char* Stage() const { return stage_; }
    uint64_t FrameCount() const { return frames_; }
    uint64_t OutputPresentCount() const { return outputPresents_; }
    uint64_t ImportCount() const { return imports_; }
    uint64_t ReuseCount() const { return reuses_; }
    uint32_t OutputRecreateCount() const { return context_.OutputRecreateCount(); }

private:
    struct FrameSlot : DirectVulkanSubmission {
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        std::shared_ptr<DirectVulkanImage> image;
        bool inFlight = false;
    };
    bool Initialize();
    bool CreatePipeline();
    bool RecreateOutput();
    bool Retire(FrameSlot& slot);
    std::shared_ptr<DirectVulkanImage> Import(OH_NativeBuffer* buffer, int32_t width, int32_t height);
    bool Fail(const char* stage, VkResult result);

    // Declared first so device destruction follows every input image reference.
    DirectVulkanContext context_;
    uint64_t outputSurfaceId_ = 0;
    uint32_t frameSlotCount_ = 2;
    std::vector<FrameSlot> slots_;
    std::unordered_map<uint32_t, std::shared_ptr<DirectVulkanImage>> cache_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    int32_t inputWidth_ = 0, inputHeight_ = 0;
    bool ready_ = false, failed_ = false, recreatePending_ = false;
    VkResult error_ = VK_SUCCESS;
    const char* stage_ = "pending";
    uint64_t frames_ = 0, completed_ = 0, outputPresents_ = 0;
    uint64_t imports_ = 0, reuses_ = 0, acquireImports_ = 0, releaseExports_ = 0;
};

} // namespace winehua::direct
