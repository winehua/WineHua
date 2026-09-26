#pragma once

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

struct OH_NativeBuffer;

namespace winehua::direct {

// Diagnostic only: imports BufferQueue images and samples their pixels on the
// GPU. Production composition and release-fence export remain separate work.
class DirectBufferImportProbe {
public:
    ~DirectBufferImportProbe();
    bool Import(OH_NativeBuffer* buffer, int32_t width, int32_t height);
    bool Sample(OH_NativeBuffer* buffer, int32_t width, int32_t height, int32_t frame);
    void NewGeneration();
    int32_t VkError() const { return static_cast<int32_t>(error_); }
    const char* Stage() const { return stage_; }
    uint32_t ImportCount() const { return imports_; }
    uint32_t ReuseCount() const { return reuses_; }
    uint32_t SampleCount() const { return samples_; }

private:
    struct Entry {
        OH_NativeBuffer* buffer = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    bool Initialize();
    bool InitializeSampler();
    bool Fail(const char* stage, VkResult result);
    void ClearCache();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, Entry> cache_;
    VkResult error_ = VK_SUCCESS;
    const char* stage_ = "pending";
    uint32_t imports_ = 0;
    uint32_t reuses_ = 0;
    uint32_t samples_ = 0;
};

} // namespace winehua::direct
