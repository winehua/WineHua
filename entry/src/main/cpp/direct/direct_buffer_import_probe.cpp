#include "direct_buffer_import_probe.h"
#include "direct_sample_spv.h"

#include <native_buffer/native_buffer.h>

#include <algorithm>
#include <cstring>

namespace winehua::direct {

DirectBufferImportProbe::~DirectBufferImportProbe()
{
    if (device_) vkDeviceWaitIdle(device_);
    ClearCache();
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
    if (instance_) vkDestroyInstance(instance_, nullptr);
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
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) return Fail("import_instance", result);
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
        if (families[i].queueCount &&
            (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
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
    };
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = fenceMode_ ? 3 : 2;
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
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
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
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &imageBarrier);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    const VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(extent), &extent);
    vkCmdDispatch(command_, 1, 1, 1);

    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.srcQueueFamilyIndex = queueFamily_;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imageBarrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (releaseFence) {
        if (!InitializeSync()) return false;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &releaseSemaphore_;
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
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquireSemaphore_;
        submit.pWaitDstStageMask = &waitStage;
    }
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
    const uint32_t expected = 0xffa55a00u | static_cast<uint8_t>(frame * 31 + 7);
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
