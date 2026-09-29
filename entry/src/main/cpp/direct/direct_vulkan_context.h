#pragma once

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <native_window/external_window.h>
#include <cstdint>
#include <vector>

struct OH_NativeBuffer;

namespace winehua::direct {

// The caller retains these images until all submissions referencing them finish.
// The context/device must outlive every image. No pixel mapping is provided.
struct DirectVulkanImage {
    DirectVulkanImage() = default;
    ~DirectVulkanImage();
    DirectVulkanImage(const DirectVulkanImage&) = delete;
    DirectVulkanImage& operator=(const DirectVulkanImage&) = delete;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    OH_NativeBuffer* native = nullptr;
    int32_t width = 0, height = 0;
};

struct DirectVulkanSubmission {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkSemaphore releaseSemaphore = VK_NULL_HANDLE;
};

struct DirectVulkanConfig {
    uint64_t outputSurfaceId = 0;
    uint32_t frameSlots = 1;
    bool enableFences = false;
    bool identityOutput = false;
};

// Single-threaded Vulkan resource owner, shared by product renderers and probes.
// Callers own pipelines, descriptors, frame state and image caches. This core
// contains no diagnostic shaders, readback buffers or CPU image upload path.
class DirectVulkanContext {
public:
    explicit DirectVulkanContext(DirectVulkanConfig config) : config_(config) {}
    ~DirectVulkanContext();
    DirectVulkanContext(const DirectVulkanContext&) = delete;
    DirectVulkanContext& operator=(const DirectVulkanContext&) = delete;

    bool Initialize();
    bool InitializeSubmissions();
    bool InitializeOutput();
    bool RecreateOutput(); // drains submitted work before replacing output resources
    bool ImportNativeBuffer(OH_NativeBuffer* buffer, int32_t width, int32_t height,
                            DirectVulkanImage& image);
    VkResult Drain(); // teardown/rebuild/error recovery only, never ordinary frames

    VkDevice Device() const { return device_; }
    VkPhysicalDevice PhysicalDevice() const { return physical_; }
    VkQueue Queue() const { return queue_; }
    uint32_t QueueFamily() const { return queueFamily_; }
    VkCommandPool CommandPool() const { return commandPool_; }
    VkSurfaceKHR OutputSurface() const { return outputSurface_; }
    const VkSwapchainKHR& OutputSwapchain() const { return outputSwapchain_; }
    VkExtent2D OutputExtent() const { return outputExtent_; }
    VkRenderPass OutputRenderPass() const { return outputRenderPass_; }
    VkFramebuffer OutputFramebuffer(uint32_t index) const { return outputFramebuffers_.at(index); }
    const VkSemaphore& OutputAcquired(uint32_t slot) const { return outputAcquired_.at(slot); }
    const VkSemaphore& OutputRendered(uint32_t image) const { return outputRendered_.at(image); }
    const DirectVulkanSubmission& Submission(uint32_t slot) const { return submissions_.at(slot); }
    PFN_vkImportSemaphoreFdKHR ImportSemaphoreFd() const { return importSemaphoreFd_; }
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFd() const { return getSemaphoreFd_; }
    VkResult Error() const { return error_; }
    const char* Stage() const { return stage_; }
    uint32_t OutputRecreateCount() const { return outputRecreates_; }

private:
    bool Fail(const char* stage, VkResult result);
    void DestroyOutput();
    DirectVulkanConfig config_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<DirectVulkanSubmission> submissions_;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd_ = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd_ = nullptr;
    OHNativeWindow* outputWindow_ = nullptr;
    VkSurfaceKHR outputSurface_ = VK_NULL_HANDLE;
    VkSwapchainKHR outputSwapchain_ = VK_NULL_HANDLE;
    VkExtent2D outputExtent_{};
    VkFormat outputFormat_ = VK_FORMAT_UNDEFINED;
    VkRenderPass outputRenderPass_ = VK_NULL_HANDLE;
    std::vector<VkImage> outputImages_;
    std::vector<VkImageView> outputViews_;
    std::vector<VkFramebuffer> outputFramebuffers_;
    std::vector<VkSemaphore> outputAcquired_, outputRendered_;
    VkResult error_ = VK_SUCCESS;
    const char* stage_ = "pending";
    uint32_t outputRecreates_ = 0;
    bool initializationAttempted_ = false, initialized_ = false;
    bool submissionsAttempted_ = false, submissionsReady_ = false;
    bool outputAttempted_ = false, outputReady_ = false;
};

} // namespace winehua::direct
