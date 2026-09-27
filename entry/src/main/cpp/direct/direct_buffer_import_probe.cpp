#include "direct_buffer_import_probe.h"
#include "direct_composite_spv.h"
#include "direct_sample_spv.h"

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace winehua::direct {

DirectBufferImportProbe::~DirectBufferImportProbe()
{
    if (device_) vkDeviceWaitIdle(device_);
    ClearCache();
    DestroyOutput();
    if (device_) {
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (acquireSemaphore_) vkDestroySemaphore(device_, acquireSemaphore_, nullptr);
        if (releaseSemaphore_) vkDestroySemaphore(device_, releaseSemaphore_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
        if (readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (outputSurface_) vkDestroySurfaceKHR(instance_, outputSurface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (outputWindow_) OH_NativeWindow_DestroyNativeWindow(outputWindow_);
}

bool DirectBufferImportProbe::Fail(const char* stage, VkResult result)
{
    stage_ = stage;
    error_ = result;
    return false;
}

void DirectBufferImportProbe::ClearCache()
{
    for (auto& [sequence, entry] : cache_) {
        (void)sequence;
        if (entry.view) vkDestroyImageView(device_, entry.view, nullptr);
        if (entry.image) vkDestroyImage(device_, entry.image, nullptr);
        if (entry.memory) vkFreeMemory(device_, entry.memory, nullptr);
        if (entry.buffer) OH_NativeBuffer_Unreference(entry.buffer);
    }
    cache_.clear();
}

void DirectBufferImportProbe::NewGeneration()
{
    ClearCache();
}

bool DirectBufferImportProbe::Initialize()
{
    if (device_) return true;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WineHua Direct D2 import";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
    if (outputSurfaceId_) {
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    }
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) return Fail("import_instance", result);
    if (outputSurfaceId_) {
        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(outputSurfaceId_,
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
    deviceInfo.enabledExtensionCount = outputSurfaceId_ ? 4 : fenceMode_ ? 3 : 2;
    deviceInfo.ppEnabledExtensionNames = extensions;
    result = vkCreateDevice(physical_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) return Fail("import_device", result);
    queueFamily_ = family;
    vkGetDeviceQueue(device_, family, 0, &queue_);
    if (!queue_) return Fail("sample_queue", VK_ERROR_INITIALIZATION_FAILED);
    if (fenceMode_) {
        importSemaphoreFd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
        getSemaphoreFd_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
        if (!importSemaphoreFd_ || !getSemaphoreFd_)
            return Fail("fence_functions", VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    return true;
}

bool DirectBufferImportProbe::InitializeSampler()
{
    VkResult result;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = 9 * sizeof(uint32_t);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(device_, &bufferInfo, nullptr, &readback_);
    if (result != VK_SUCCESS) return Fail("sample_buffer", result);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, readback_, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX)
        return Fail("sample_host_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(device_, &allocation, nullptr, &readbackMemory_);
    if (result != VK_SUCCESS) return Fail("sample_allocate", result);
    result = vkBindBufferMemory(device_, readback_, readbackMemory_, 0);
    if (result != VK_SUCCESS) return Fail("sample_bind", result);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    result = vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_);
    if (result != VK_SUCCESS) return Fail("sample_sampler", result);
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = outputSurfaceId_ ?
        (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_);
    if (result != VK_SUCCESS) return Fail("sample_descriptor_layout", result);
    VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    result = vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
    if (result != VK_SUCCESS) return Fail("sample_descriptor_pool", result);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorLayout_;
    result = vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_);
    if (result != VK_SUCCESS) return Fail("sample_descriptor_set", result);
    VkPushConstantRange sizeRange{};
    sizeRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    sizeRange.size = sizeof(VkExtent2D);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &sizeRange;
    result = vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_);
    if (result != VK_SUCCESS) return Fail("sample_pipeline_layout", result);
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(kDirectSampleSpv);
    moduleInfo.pCode = kDirectSampleSpv;
    result = vkCreateShaderModule(device_, &moduleInfo, nullptr, &module);
    if (result != VK_SUCCESS) return Fail("sample_shader_module", result);
    VkPipelineShaderStageCreateInfo shaderStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = module;
    shaderStage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = pipelineLayout_;
    result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    if (result != VK_SUCCESS) return Fail("sample_pipeline", result);
    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = queueFamily_;
    result = vkCreateCommandPool(device_, &commandPoolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) return Fail("sample_command_pool", result);
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device_, &commandInfo, &command_);
    if (result != VK_SUCCESS) return Fail("sample_command_buffer", result);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(device_, &fenceInfo, nullptr, &fence_);
    if (result != VK_SUCCESS) return Fail("sample_fence", result);
    return true;
}

bool DirectBufferImportProbe::InitializeSync()
{
    if (acquireSemaphore_) return true;
    VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &exportInfo;
    VkResult result = vkCreateSemaphore(device_, &createInfo, nullptr, &acquireSemaphore_);
    if (result != VK_SUCCESS) return Fail("fence_acquire_semaphore", result);
    result = vkCreateSemaphore(device_, &createInfo, nullptr, &releaseSemaphore_);
    if (result != VK_SUCCESS) return Fail("fence_release_semaphore", result);
    return true;
}

void DirectBufferImportProbe::DestroyOutput()
{
    if (!device_) return;
    for (VkSemaphore semaphore : outputRendered_)
        if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
    outputRendered_.clear();
    if (outputAcquired_) vkDestroySemaphore(device_, outputAcquired_, nullptr);
    outputAcquired_ = VK_NULL_HANDLE;
    for (VkFramebuffer framebuffer : outputFramebuffers_)
        if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    outputFramebuffers_.clear();
    if (outputPipeline_) vkDestroyPipeline(device_, outputPipeline_, nullptr);
    outputPipeline_ = VK_NULL_HANDLE;
    if (outputRenderPass_) vkDestroyRenderPass(device_, outputRenderPass_, nullptr);
    outputRenderPass_ = VK_NULL_HANDLE;
    for (VkImageView view : outputViews_)
        if (view) vkDestroyImageView(device_, view, nullptr);
    outputViews_.clear();
    outputImages_.clear();
    if (outputSwapchain_) vkDestroySwapchainKHR(device_, outputSwapchain_, nullptr);
    outputSwapchain_ = VK_NULL_HANDLE;
}

bool DirectBufferImportProbe::RecreateOutput()
{
    if (!outputSurfaceId_ || !outputSwapchain_)
        return Fail("composite_recreate_input", VK_ERROR_INITIALIZATION_FAILED);
    VkResult result = vkDeviceWaitIdle(device_);
    if (result != VK_SUCCESS) return Fail("composite_recreate_idle", result);
    DestroyOutput();
    if (!InitializeOutput()) return false;
    ++outputRecreates_;
    return true;
}

bool DirectBufferImportProbe::InitializeOutput()
{
    if (!outputSurfaceId_) return true;
    if (outputSwapchain_) return true;
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
    swapInfo.preTransform = caps.currentTransform;
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
    VkShaderModule modules[2]{};
    const uint32_t* code[] = {kDirectCompositeVertSpv, kDirectCompositeFragSpv};
    const size_t codeSize[] = {sizeof(kDirectCompositeVertSpv),
                               sizeof(kDirectCompositeFragSpv)};
    for (uint32_t i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = codeSize[i];
        moduleInfo.pCode = code[i];
        result = vkCreateShaderModule(device_, &moduleInfo, nullptr, &modules[i]);
        if (result != VK_SUCCESS) {
            if (modules[0]) vkDestroyShaderModule(device_, modules[0], nullptr);
            return Fail("composite_shader_module", result);
        }
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(outputExtent_.width),
                        static_cast<float>(outputExtent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, outputExtent_};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertex;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = outputRenderPass_;
    result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &outputPipeline_);
    for (VkShaderModule module : modules) vkDestroyShaderModule(device_, module, nullptr);
    if (result != VK_SUCCESS) return Fail("composite_pipeline", result);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &outputAcquired_);
    if (result != VK_SUCCESS) return Fail("composite_acquire_semaphore", result);
    outputRendered_.resize(imageCount, VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : outputRendered_) {
        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        if (result != VK_SUCCESS) return Fail("composite_render_semaphore", result);
    }
    return true;
}

bool DirectBufferImportProbe::Import(OH_NativeBuffer* buffer, int32_t width, int32_t height)
{
    if (!buffer || width <= 0 || height <= 0)
        return Fail("import_input", VK_ERROR_INITIALIZATION_FAILED);
    if (!Initialize()) return false;
    const uint32_t sequence = OH_NativeBuffer_GetSeqNum(buffer);
    auto found = cache_.find(sequence);
    if (found != cache_.end()) {
        if (found->second.buffer != buffer)
            return Fail("import_seq_alias", VK_ERROR_INITIALIZATION_FAILED);
        ++reuses_;
        stage_ = "reused";
        return true;
    }

    VkNativeBufferFormatPropertiesOHOS format{
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS properties{VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    properties.pNext = &format;
    VkResult result = vkGetNativeBufferPropertiesOHOS(device_, buffer, &properties);
    if (result != VK_SUCCESS) return Fail("native_buffer_properties", result);
    if (format.format != VK_FORMAT_R8G8B8A8_UNORM || !properties.allocationSize ||
        !properties.memoryTypeBits)
        return Fail("native_buffer_format", VK_ERROR_FORMAT_NOT_SUPPORTED);

    Entry entry{};
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format.format;
    imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(device_, &imageInfo, nullptr, &entry.image);
    if (result != VK_SUCCESS) return Fail("import_image", result);
    auto cleanup = [&] {
        if (entry.view) vkDestroyImageView(device_, entry.view, nullptr);
        if (entry.image) vkDestroyImage(device_, entry.image, nullptr);
        if (entry.memory) vkFreeMemory(device_, entry.memory, nullptr);
    };
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, entry.image, &requirements);
    const uint32_t compatibleTypes = properties.memoryTypeBits & requirements.memoryTypeBits;
    if (!compatibleTypes) {
        cleanup();
        return Fail("import_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    uint32_t typeIndex = 0;
    while (!(compatibleTypes & (1u << typeIndex))) ++typeIndex;
    VkImportNativeBufferInfoOHOS nativeImport{VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
    nativeImport.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &nativeImport;
    dedicated.image = entry.image;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &dedicated;
    allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = typeIndex;
    result = vkAllocateMemory(device_, &allocation, nullptr, &entry.memory);
    if (result != VK_SUCCESS) {
        cleanup();
        return Fail("import_allocate", result);
    }
    result = vkBindImageMemory(device_, entry.image, entry.memory, 0);
    if (result != VK_SUCCESS) {
        cleanup();
        return Fail("import_bind", result);
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = entry.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device_, &viewInfo, nullptr, &entry.view);
    if (result != VK_SUCCESS) {
        cleanup();
        return Fail("import_view", result);
    }
    if (OH_NativeBuffer_Reference(buffer) != 0) {
        cleanup();
        return Fail("import_reference", VK_ERROR_INITIALIZATION_FAILED);
    }
    entry.buffer = buffer;
    cache_.emplace(sequence, entry);
    ++imports_;
    stage_ = "imported";
    error_ = VK_SUCCESS;
    return true;
}

bool DirectBufferImportProbe::Sample(OH_NativeBuffer* buffer, int32_t width,
                                     int32_t height, int32_t frame)
{
    if (frame < 0) return Fail("sample_frame", VK_ERROR_INITIALIZATION_FAILED);
    return RecordAndSubmit(buffer, width, height, nullptr, nullptr) && FinishSample(frame);
}

bool DirectBufferImportProbe::SubmitSampleWithFences(OH_NativeBuffer* buffer,
                                                     int32_t width, int32_t height,
                                                     int* acquireFence, int* releaseFence)
{
    if (!fenceMode_ || !acquireFence || !releaseFence)
        return Fail("fence_mode_input", VK_ERROR_INITIALIZATION_FAILED);
    *releaseFence = -1;
    return RecordAndSubmit(buffer, width, height, acquireFence, releaseFence);
}

bool DirectBufferImportProbe::RecordAndSubmit(OH_NativeBuffer* buffer, int32_t width,
                                               int32_t height, int* acquireFence,
                                               int* releaseFence)
{
    if (!buffer || width <= 0 || height <= 0)
        return Fail("sample_input", VK_ERROR_INITIALIZATION_FAILED);
    const auto found = cache_.find(OH_NativeBuffer_GetSeqNum(buffer));
    if (found == cache_.end() || found->second.buffer != buffer)
        return Fail("sample_not_imported", VK_ERROR_INITIALIZATION_FAILED);
    if (!pipeline_ && !InitializeSampler()) return false;
    if (outputSurfaceId_ && !InitializeOutput()) return false;
    uint32_t outputImageIndex = 0;
    if (outputSurfaceId_) {
        VkResult acquired = vkAcquireNextImageKHR(device_, outputSwapchain_, 5'000'000'000ULL,
                                                    outputAcquired_, VK_NULL_HANDLE,
                                                    &outputImageIndex);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return Fail("composite_acquire", acquired);
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler_;
    imageInfo.imageView = found->second.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = readback_;
    bufferInfo.range = 9 * sizeof(uint32_t);
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    VkResult result = vkResetCommandBuffer(command_, 0);
    if (result != VK_SUCCESS) return Fail("sample_command_reset", result);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_, &beginInfo);
    if (result != VK_SUCCESS) return Fail("sample_command_begin", result);
    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    imageBarrier.dstQueueFamilyIndex = queueFamily_;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imageBarrier.image = found->second.image;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             (outputSurfaceId_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                         0, 0, nullptr, 0, nullptr,
                         1, &imageBarrier);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    const VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(extent), &extent);
    vkCmdDispatch(command_, 1, 1, 1);
    if (outputSurfaceId_) {
        VkClearValue clear{};
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = outputRenderPass_;
        render.framebuffer = outputFramebuffers_[outputImageIndex];
        render.renderArea = {{0, 0}, outputExtent_};
        render.clearValueCount = 1;
        render.pClearValues = &clear;
        vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipeline_);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                0, 1, &descriptorSet_, 0, nullptr);
        vkCmdDraw(command_, 3, 1, 0, 0);
        vkCmdEndRenderPass(command_);
    }

    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.srcQueueFamilyIndex = queueFamily_;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imageBarrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             (outputSurfaceId_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &imageBarrier);
    VkBufferMemoryBarrier readbackBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    readbackBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    readbackBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readbackBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readbackBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readbackBarrier.buffer = readback_;
    readbackBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &readbackBarrier,
                         0, nullptr);
    result = vkEndCommandBuffer(command_);
    if (result != VK_SUCCESS) return Fail("sample_command_end", result);
    result = vkResetFences(device_, 1, &fence_);
    if (result != VK_SUCCESS) return Fail("sample_fence_reset", result);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    VkSemaphore waitSemaphores[2]{};
    VkPipelineStageFlags waitStages[2]{};
    VkSemaphore signalSemaphores[2]{};
    if (releaseFence) {
        if (!InitializeSync()) return false;
        signalSemaphores[submit.signalSemaphoreCount++] = releaseSemaphore_;
    }
    if (acquireFence && *acquireFence >= 0) {
        VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        importInfo.semaphore = acquireSemaphore_;
        importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        importInfo.fd = *acquireFence;
        result = importSemaphoreFd_(device_, &importInfo);
        if (result != VK_SUCCESS) return Fail("fence_import", result);
        *acquireFence = -1; // Vulkan owns the fd after a successful import.
        ++acquireImports_;
        waitSemaphores[submit.waitSemaphoreCount] = acquireSemaphore_;
        waitStages[submit.waitSemaphoreCount++] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    if (outputSurfaceId_) {
        waitSemaphores[submit.waitSemaphoreCount] = outputAcquired_;
        waitStages[submit.waitSemaphoreCount++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        signalSemaphores[submit.signalSemaphoreCount++] = outputRendered_[outputImageIndex];
    }
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitStages;
    submit.pSignalSemaphores = signalSemaphores;
    result = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (result != VK_SUCCESS) return Fail("sample_submit", result);
    if (releaseFence) {
        VkSemaphoreGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        exportInfo.semaphore = releaseSemaphore_;
        exportInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        result = getSemaphoreFd_(device_, &exportInfo, releaseFence);
        if (result != VK_SUCCESS) {
            // Without a release fd, finish the submitted work before the caller
            // gives the BufferQueue its buffer back.
            vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ULL);
            return Fail("fence_export", result);
        }
        ++releaseExports_;
    }
    if (outputSurfaceId_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &outputRendered_[outputImageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &outputSwapchain_;
        present.pImageIndices = &outputImageIndex;
        result = vkQueuePresentKHR(queue_, &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ULL);
            if (releaseFence && *releaseFence >= 0) {
                close(*releaseFence);
                *releaseFence = -1;
            }
            return Fail("composite_present", result);
        }
        ++outputPresents_;
    }
    stage_ = "sample_submitted";
    return true;
}

bool DirectBufferImportProbe::FinishSample(int32_t frame)
{
    if (frame < 0) return Fail("sample_frame", VK_ERROR_INITIALIZATION_FAILED);
    VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ULL);
    if (result != VK_SUCCESS) return Fail("sample_wait", result);
    void* mapped = nullptr;
    result = vkMapMemory(device_, readbackMemory_, 0, 9 * sizeof(uint32_t), 0, &mapped);
    if (result != VK_SUCCESS) return Fail("sample_map", result);
    const uint32_t expected = 0xffa55a00u | static_cast<uint8_t>((frame % 8) * 31 + 7);
    bool matches = true;
    for (uint32_t i = 0; i < 9; ++i) {
        if (static_cast<const uint32_t*>(mapped)[i] != expected) matches = false;
    }
    vkUnmapMemory(device_, readbackMemory_);
    if (!matches) return Fail("gpu_sample_mismatch", VK_ERROR_UNKNOWN);
    ++samples_;
    stage_ = "sampled";
    error_ = VK_SUCCESS;
    return true;
}

} // namespace winehua::direct
