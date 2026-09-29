#include "direct_vulkan_context.h"
#include <native_buffer/native_buffer.h>
#include <algorithm>

namespace winehua::direct {

DirectVulkanImage::~DirectVulkanImage()
{
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    if (native) OH_NativeBuffer_Unreference(native);
}

DirectVulkanContext::~DirectVulkanContext()
{
    if (device_) {
        Drain();
        DestroyOutput();
        for (const auto& slot : submissions_) {
            if (slot.fence) vkDestroyFence(device_, slot.fence, nullptr);
            if (slot.acquireSemaphore) vkDestroySemaphore(device_, slot.acquireSemaphore, nullptr);
            if (slot.releaseSemaphore) vkDestroySemaphore(device_, slot.releaseSemaphore, nullptr);
        }
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (outputSurface_) vkDestroySurfaceKHR(instance_, outputSurface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (outputWindow_) OH_NativeWindow_DestroyNativeWindow(outputWindow_);
}

VkResult DirectVulkanContext::Drain()
{
    return device_ ? vkDeviceWaitIdle(device_) : VK_SUCCESS;
}

bool DirectVulkanContext::Fail(const char* stage, VkResult result)
{
    stage_ = stage;
    error_ = result;
    return false;
}

bool DirectVulkanContext::Initialize()
{
    if (initialized_) return true;
    if (initializationAttempted_) return false;
    initializationAttempted_ = true;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WineHua Direct Vulkan";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
    if (config_.outputSurfaceId) {
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    }
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) return Fail("import_instance", result);
    if (config_.outputSurfaceId) {
        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(config_.outputSurfaceId,
                                                             &outputWindow_) != 0 || !outputWindow_)
            return Fail("composite_window", VK_ERROR_INITIALIZATION_FAILED);
        VkSurfaceCreateInfoOHOS surfaceInfo{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
        surfaceInfo.window = outputWindow_;
        result = vkCreateSurfaceOHOS(instance_, &surfaceInfo, nullptr, &outputSurface_);
        if (result != VK_SUCCESS) return Fail("composite_surface", result);
    }
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (result != VK_SUCCESS || !count || count > 16)
        return Fail("import_physical_count",
                    result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
    VkPhysicalDevice devices[16]{};
    result = vkEnumeratePhysicalDevices(instance_, &count, devices);
    if (result != VK_SUCCESS) return Fail("import_physical", result);
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            physical_ = devices[i];
            break;
        }
    }
    if (!physical_) return Fail("import_native_device", VK_ERROR_INITIALIZATION_FAILED);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
    if (!familyCount || familyCount > 32)
        return Fail("import_queue_count", VK_ERROR_INITIALIZATION_FAILED);
    VkQueueFamilyProperties families[32]{};
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkBool32 present = VK_FALSE;
        if (outputSurface_ &&
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_, i, outputSurface_, &present) != VK_SUCCESS)
            continue;
        if (families[i].queueCount &&
            (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
            (!outputSurface_ || present)) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) return Fail("import_queue", VK_ERROR_INITIALIZATION_FAILED);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* extensions[] = {
        VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME,
        "VK_EXT_queue_family_foreign",
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = config_.outputSurfaceId ? 4 : config_.enableFences ? 3 : 2;
    deviceInfo.ppEnabledExtensionNames = extensions;
    result = vkCreateDevice(physical_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) return Fail("import_device", result);
    queueFamily_ = family;
    vkGetDeviceQueue(device_, family, 0, &queue_);
    if (!queue_) return Fail("sample_queue", VK_ERROR_INITIALIZATION_FAILED);
    if (config_.enableFences) {
        importSemaphoreFd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
        getSemaphoreFd_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
        if (!importSemaphoreFd_ || !getSemaphoreFd_)
            return Fail("fence_functions", VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    initialized_ = true;
    return true;
}

bool DirectVulkanContext::InitializeSubmissions()
{
    if (submissionsReady_) return true;
    if (submissionsAttempted_) return false;
    submissionsAttempted_ = true;
    if (!Initialize()) return false;
    if (config_.frameSlots == 0 || config_.frameSlots > 3)
        return Fail("sample_slot_count", VK_ERROR_INITIALIZATION_FAILED);
    submissions_.resize(config_.frameSlots);
    VkResult result;
    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = queueFamily_;
    result = vkCreateCommandPool(device_, &commandPoolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) return Fail("sample_command_pool", result);
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = config_.frameSlots;
    std::vector<VkCommandBuffer> commands(config_.frameSlots, VK_NULL_HANDLE);
    result = vkAllocateCommandBuffers(device_, &commandInfo, commands.data());
    if (result != VK_SUCCESS) return Fail("sample_command_buffer", result);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t i = 0; i < config_.frameSlots; ++i) {
        DirectVulkanSubmission& slot = submissions_[i];
        slot.command = commands[i];
        result = vkCreateFence(device_, &fenceInfo, nullptr, &slot.fence);
        if (result != VK_SUCCESS) return Fail("sample_fence", result);
    }

    if (config_.enableFences) {
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &exportInfo;
    for (DirectVulkanSubmission& slot : submissions_) {
        VkResult result = vkCreateSemaphore(device_, &createInfo, nullptr, &slot.acquireSemaphore);
        if (result != VK_SUCCESS) return Fail("fence_acquire_semaphore", result);
        result = vkCreateSemaphore(device_, &createInfo, nullptr, &slot.releaseSemaphore);
        if (result != VK_SUCCESS) return Fail("fence_release_semaphore", result);
    }
    }
    submissionsReady_ = true;
    return true;
}

void DirectVulkanContext::DestroyOutput()
{
    outputReady_ = false;
    outputAttempted_ = false;
    if (!device_) return;
    for (VkSemaphore semaphore : outputRendered_)
        if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
    outputRendered_.clear();
    for (VkSemaphore semaphore : outputAcquired_)
        if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
    outputAcquired_.clear();
    for (VkFramebuffer framebuffer : outputFramebuffers_)
        if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    outputFramebuffers_.clear();
    if (outputRenderPass_) vkDestroyRenderPass(device_, outputRenderPass_, nullptr);
    outputRenderPass_ = VK_NULL_HANDLE;
    for (VkImageView view : outputViews_)
        if (view) vkDestroyImageView(device_, view, nullptr);
    outputViews_.clear();
    outputImages_.clear();
    if (outputSwapchain_) vkDestroySwapchainKHR(device_, outputSwapchain_, nullptr);
    outputSwapchain_ = VK_NULL_HANDLE;
}

bool DirectVulkanContext::RecreateOutput()
{
    if (!config_.outputSurfaceId || !outputReady_)
        return Fail("composite_recreate_input", VK_ERROR_INITIALIZATION_FAILED);
    VkResult result = vkDeviceWaitIdle(device_);
    if (result != VK_SUCCESS) return Fail("composite_recreate_idle", result);
    DestroyOutput();
    if (!InitializeOutput()) return false;
    ++outputRecreates_;
    return true;
}

bool DirectVulkanContext::InitializeOutput()
{
    if (!config_.outputSurfaceId) return true;
    if (outputReady_) return true;
    if (outputAttempted_) return false;
    outputAttempted_ = true;
    if (!InitializeSubmissions()) return false;
    VkResult result;
    VkSurfaceCapabilitiesKHR caps{};
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, outputSurface_, &caps);
    if (result != VK_SUCCESS) return Fail("composite_capabilities", result);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return Fail("composite_color_usage", VK_ERROR_FORMAT_NOT_SUPPORTED);
    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, outputSurface_, &formatCount, nullptr);
    if (result != VK_SUCCESS || !formatCount)
        return Fail("composite_format_count", VK_ERROR_FORMAT_NOT_SUPPORTED);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, outputSurface_, &formatCount,
                                                  formats.data());
    if (result != VK_SUCCESS) return Fail("composite_formats", result);
    VkSurfaceFormatKHR format = formats[0];
    for (const auto& candidate : formats) {
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = candidate;
            break;
        }
    }
    if (format.format != VK_FORMAT_R8G8B8A8_UNORM &&
        format.format != VK_FORMAT_B8G8R8A8_UNORM)
        return Fail("composite_rgba_format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    outputFormat_ = format.format;
    outputExtent_ = caps.currentExtent;
    if (outputExtent_.width == UINT32_MAX) {
        outputExtent_.width = std::clamp(320u, caps.minImageExtent.width,
                                         caps.maxImageExtent.width);
        outputExtent_.height = std::clamp(240u, caps.minImageExtent.height,
                                          caps.maxImageExtent.height);
    }
    if (!outputExtent_.width || !outputExtent_.height)
        return Fail("composite_extent", VK_ERROR_INITIALIZATION_FAILED);
    uint32_t count = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        const VkCompositeAlphaFlagBitsKHR choices[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (auto choice : choices)
            if (caps.supportedCompositeAlpha & choice) { alpha = choice; break; }
    }
    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapInfo.surface = outputSurface_;
    swapInfo.minImageCount = count;
    swapInfo.imageFormat = format.format;
    swapInfo.imageColorSpace = format.colorSpace;
    swapInfo.imageExtent = outputExtent_;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (config_.identityOutput && !(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        return Fail("composite_identity_transform", VK_ERROR_FEATURE_NOT_PRESENT);
    swapInfo.preTransform = config_.identityOutput ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    swapInfo.compositeAlpha = alpha;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    result = vkCreateSwapchainKHR(device_, &swapInfo, nullptr, &outputSwapchain_);
    if (result != VK_SUCCESS) return Fail("composite_swapchain", result);
    uint32_t imageCount = 0;
    result = vkGetSwapchainImagesKHR(device_, outputSwapchain_, &imageCount, nullptr);
    if (result != VK_SUCCESS || !imageCount)
        return Fail("composite_image_count", VK_ERROR_INITIALIZATION_FAILED);
    outputImages_.resize(imageCount);
    result = vkGetSwapchainImagesKHR(device_, outputSwapchain_, &imageCount, outputImages_.data());
    if (result != VK_SUCCESS) return Fail("composite_images", result);
    outputViews_.resize(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = outputImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = outputFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device_, &viewInfo, nullptr, &outputViews_[i]);
        if (result != VK_SUCCESS) return Fail("composite_view", result);
    }
    VkAttachmentDescription attachment{};
    attachment.format = outputFormat_;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderInfo.attachmentCount = 1;
    renderInfo.pAttachments = &attachment;
    renderInfo.subpassCount = 1;
    renderInfo.pSubpasses = &subpass;
    result = vkCreateRenderPass(device_, &renderInfo, nullptr, &outputRenderPass_);
    if (result != VK_SUCCESS) return Fail("composite_render_pass", result);
    outputFramebuffers_.resize(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = outputRenderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &outputViews_[i];
        framebufferInfo.width = outputExtent_.width;
        framebufferInfo.height = outputExtent_.height;
        framebufferInfo.layers = 1;
        result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                                     &outputFramebuffers_[i]);
        if (result != VK_SUCCESS) return Fail("composite_framebuffer", result);
    }
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    outputAcquired_.resize(config_.frameSlots, VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : outputAcquired_) {
        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        if (result != VK_SUCCESS) return Fail("composite_acquire_semaphore", result);
    }
    outputRendered_.resize(imageCount, VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : outputRendered_) {
        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        if (result != VK_SUCCESS) return Fail("composite_render_semaphore", result);
    }
    outputReady_ = true;
    return true;
}

bool DirectVulkanContext::ImportNativeBuffer(OH_NativeBuffer* buffer, int32_t width,
                                              int32_t height, DirectVulkanImage& image)
{
    if (!buffer || width <= 0 || height <= 0 || image.image || image.memory || image.native)
        return Fail("import_input", VK_ERROR_INITIALIZATION_FAILED);
    if (!Initialize()) return false;
    const auto getProperties = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(
        vkGetDeviceProcAddr(device_, "vkGetNativeBufferPropertiesOHOS"));
    if (!getProperties) return Fail("native_buffer_properties_function", VK_ERROR_EXTENSION_NOT_PRESENT);
    VkNativeBufferFormatPropertiesOHOS format{VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS properties{VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    properties.pNext = &format;
    VkResult result = getProperties(device_, buffer, &properties);
    if (result != VK_SUCCESS) return Fail("native_buffer_properties", result);
    if (format.format == VK_FORMAT_UNDEFINED || !properties.allocationSize || !properties.memoryTypeBits ||
        !(format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        return Fail("native_buffer_format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    image.device = device_;
    image.width = width;
    image.height = height;
    image.format = format.format;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format.format;
    info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateImage(device_, &info, nullptr, &image.image);
    if (result != VK_SUCCESS) return Fail("import_image", result);
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image.image, &requirements);
    const uint32_t bits = requirements.memoryTypeBits & properties.memoryTypeBits;
    if (!bits) return Fail("import_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
    uint32_t type = 0;
    while (!(bits & (1u << type))) ++type;
    VkImportNativeBufferInfoOHOS nativeImport{VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
    nativeImport.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &nativeImport; dedicated.image = image.image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &dedicated; allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(device_, &allocation, nullptr, &image.memory);
    if (result != VK_SUCCESS) return Fail("import_allocate", result);
    result = vkBindImageMemory(device_, image.image, image.memory, 0);
    if (result != VK_SUCCESS) return Fail("import_bind", result);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = vkCreateImageView(device_, &view, nullptr, &image.view);
    if (result != VK_SUCCESS) return Fail("import_view", result);
    if (OH_NativeBuffer_Reference(buffer) != 0)
        return Fail("import_reference", VK_ERROR_INITIALIZATION_FAILED);
    image.native = buffer;
    stage_ = "imported"; error_ = VK_SUCCESS;
    return true;
}

} // namespace winehua::direct
