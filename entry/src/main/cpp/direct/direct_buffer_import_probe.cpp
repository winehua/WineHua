#include "direct_buffer_import_probe.h"
#include "direct_composite_spv.h"
#include "direct_sample_spv.h"
#include <native_buffer/native_buffer.h>
#include <unistd.h>

namespace winehua::direct {

DirectBufferImportProbe::~DirectBufferImportProbe()
{
    context_.Drain();
    for (auto& slot : frameSlots_) slot.imageReference.reset();
    ClearCache();
    DestroyOutput();
    if (context_.Device()) {
        for (auto& slot : frameSlots_) {
            if (slot.readback) vkDestroyBuffer(context_.Device(), slot.readback, nullptr);
            if (slot.readbackMemory) vkFreeMemory(context_.Device(), slot.readbackMemory, nullptr);
        }
        if (pipeline_) vkDestroyPipeline(context_.Device(), pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(context_.Device(), pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(context_.Device(), descriptorPool_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(context_.Device(), descriptorLayout_, nullptr);
        if (sampler_) vkDestroySampler(context_.Device(), sampler_, nullptr);
    }
}

bool DirectBufferImportProbe::Fail(const char* stage, VkResult result)
{
    stage_ = stage;
    error_ = result;
    return false;
}

void DirectBufferImportProbe::ClearCache()
{
    cache_.clear();
}

void DirectBufferImportProbe::NewGeneration()
{
    ClearCache();
}

bool DirectBufferImportProbe::Initialize()
{
    return context_.Initialize() || Fail(context_.Stage(), context_.Error());
}

bool DirectBufferImportProbe::InitializeSampler()
{
    VkResult result;
    if (frameSlotCount_ == 0 || frameSlotCount_ > 3)
        return Fail("sample_slot_count", VK_ERROR_INITIALIZATION_FAILED);
    if (!context_.InitializeSubmissions()) return Fail(context_.Stage(), context_.Error());
    frameSlots_.resize(frameSlotCount_);
    if (verifyPixels_) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(context_.PhysicalDevice(), &memoryProperties);
        for (FrameSlot& slot : frameSlots_) {
            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.size = 9 * sizeof(uint32_t);
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            result = vkCreateBuffer(context_.Device(), &bufferInfo, nullptr, &slot.readback);
            if (result != VK_SUCCESS) return Fail("sample_buffer", result);
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(context_.Device(), slot.readback, &requirements);
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
            result = vkAllocateMemory(context_.Device(), &allocation, nullptr, &slot.readbackMemory);
            if (result != VK_SUCCESS) return Fail("sample_allocate", result);
            result = vkBindBufferMemory(context_.Device(), slot.readback, slot.readbackMemory, 0);
            if (result != VK_SUCCESS) return Fail("sample_bind", result);
        }
    }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    result = vkCreateSampler(context_.Device(), &samplerInfo, nullptr, &sampler_);
    if (result != VK_SUCCESS) return Fail("sample_sampler", result);
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = (verifyPixels_ ? VK_SHADER_STAGE_COMPUTE_BIT : 0) |
                            (outputSurfaceId_ ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);
    if (!bindings[0].stageFlags) bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = verifyPixels_ ? 2 : 1;
    layoutInfo.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(context_.Device(), &layoutInfo, nullptr, &descriptorLayout_);
    if (result != VK_SUCCESS) return Fail("sample_descriptor_layout", result);
    VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameSlotCount_},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameSlotCount_},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = frameSlotCount_;
    poolInfo.poolSizeCount = verifyPixels_ ? 2 : 1;
    poolInfo.pPoolSizes = poolSizes;
    result = vkCreateDescriptorPool(context_.Device(), &poolInfo, nullptr, &descriptorPool_);
    if (result != VK_SUCCESS) return Fail("sample_descriptor_pool", result);
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool_;
    std::vector<VkDescriptorSetLayout> layouts(frameSlotCount_, descriptorLayout_);
    std::vector<VkDescriptorSet> sets(frameSlotCount_, VK_NULL_HANDLE);
    setInfo.descriptorSetCount = frameSlotCount_;
    setInfo.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(context_.Device(), &setInfo, sets.data());
    if (result != VK_SUCCESS) return Fail("sample_descriptor_set", result);
    for (uint32_t i = 0; i < frameSlotCount_; ++i) frameSlots_[i].descriptorSet = sets[i];
    VkPushConstantRange sizeRange{};
    sizeRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    sizeRange.size = sizeof(VkExtent2D);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = verifyPixels_ ? 1 : 0;
    pipelineLayoutInfo.pPushConstantRanges = verifyPixels_ ? &sizeRange : nullptr;
    result = vkCreatePipelineLayout(context_.Device(), &pipelineLayoutInfo, nullptr, &pipelineLayout_);
    if (result != VK_SUCCESS) return Fail("sample_pipeline_layout", result);
    if (verifyPixels_) {
        VkShaderModule module = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(kDirectSampleSpv);
        moduleInfo.pCode = kDirectSampleSpv;
        result = vkCreateShaderModule(context_.Device(), &moduleInfo, nullptr, &module);
        if (result != VK_SUCCESS) return Fail("sample_shader_module", result);
        VkPipelineShaderStageCreateInfo shaderStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStage.module = module;
        shaderStage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = shaderStage;
        pipelineInfo.layout = pipelineLayout_;
        result = vkCreateComputePipelines(context_.Device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
        vkDestroyShaderModule(context_.Device(), module, nullptr);
        if (result != VK_SUCCESS) return Fail("sample_pipeline", result);
    }
    for (uint32_t i = 0; i < frameSlotCount_; ++i)
        static_cast<DirectVulkanSubmission&>(frameSlots_[i]) = context_.Submission(i);
    return true;
}

bool DirectBufferImportProbe::InitializeSync()
{
    return context_.InitializeSubmissions() || Fail(context_.Stage(), context_.Error());
}

void DirectBufferImportProbe::DestroyOutput()
{
    if (outputPipeline_) vkDestroyPipeline(context_.Device(), outputPipeline_, nullptr);
    outputPipeline_ = VK_NULL_HANDLE;
}

bool DirectBufferImportProbe::RecreateOutput()
{
    if (!context_.RecreateOutput()) return Fail(context_.Stage(), context_.Error());
    DestroyOutput();
    return InitializeOutput();
}

bool DirectBufferImportProbe::InitializeOutput()
{
    if (!outputSurfaceId_ || outputPipeline_) return true;
    if (!context_.InitializeOutput()) return Fail(context_.Stage(), context_.Error());
    VkResult result;
    VkShaderModule modules[2]{};
    const uint32_t* code[] = {kDirectCompositeVertSpv, kDirectCompositeFragSpv};
    const size_t codeSize[] = {sizeof(kDirectCompositeVertSpv),
                               sizeof(kDirectCompositeFragSpv)};
    for (uint32_t i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = codeSize[i];
        moduleInfo.pCode = code[i];
        result = vkCreateShaderModule(context_.Device(), &moduleInfo, nullptr, &modules[i]);
        if (result != VK_SUCCESS) {
            if (modules[0]) vkDestroyShaderModule(context_.Device(), modules[0], nullptr);
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
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(context_.OutputExtent().width),
                        static_cast<float>(context_.OutputExtent().height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, context_.OutputExtent()};
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
    pipelineInfo.renderPass = context_.OutputRenderPass();
    result = vkCreateGraphicsPipelines(context_.Device(), VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &outputPipeline_);
    for (VkShaderModule module : modules) vkDestroyShaderModule(context_.Device(), module, nullptr);
    if (result != VK_SUCCESS) return Fail("composite_pipeline", result);
    return true;
}

bool DirectBufferImportProbe::Import(OH_NativeBuffer* buffer, int32_t width, int32_t height)
{
    if (!buffer || width <= 0 || height <= 0)
        return Fail("import_input", VK_ERROR_INITIALIZATION_FAILED);
    const uint32_t sequence = OH_NativeBuffer_GetSeqNum(buffer);
    const auto found = cache_.find(sequence);
    if (found != cache_.end()) {
        if (found->second->native != buffer || found->second->width != width || found->second->height != height)
            return Fail("import_seq_alias", VK_ERROR_INITIALIZATION_FAILED);
        ++reuses_; stage_ = "reused"; return true;
    }
    auto image = std::make_shared<DirectVulkanImage>();
    if (!context_.ImportNativeBuffer(buffer, width, height, *image))
        return Fail(context_.Stage(), context_.Error());
    // Fixed diagnostic patterns are RGBA8; product consumers use the format
    // supplied by NativeBuffer and do not perform this diagnostic check.
    if (verifyPixels_ && image->format != VK_FORMAT_R8G8B8A8_UNORM)
        return Fail("native_buffer_format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    cache_.emplace(sequence, std::move(image));
    ++imports_; stage_ = "imported"; error_ = VK_SUCCESS;
    return true;
}

bool DirectBufferImportProbe::Sample(OH_NativeBuffer* buffer, int32_t width,
                                     int32_t height, int32_t frame)
{
    if (frame < 0) return Fail("sample_frame", VK_ERROR_INITIALIZATION_FAILED);
    return RecordAndSubmit(buffer, width, height, frame, nullptr, nullptr) && FinishSample(frame);
}

bool DirectBufferImportProbe::SubmitSampleWithFences(OH_NativeBuffer* buffer,
                                                     int32_t width, int32_t height,
                                                     int32_t frame, int* acquireFence,
                                                     int* releaseFence)
{
    if (!fenceMode_ || frame < 0 || !acquireFence || !releaseFence)
        return Fail("fence_mode_input", VK_ERROR_INITIALIZATION_FAILED);
    *releaseFence = -1;
    return RecordAndSubmit(buffer, width, height, frame, acquireFence, releaseFence);
}

bool DirectBufferImportProbe::RecordAndSubmit(OH_NativeBuffer* buffer, int32_t width,
                                               int32_t height, int32_t frame, int* acquireFence,
                                               int* releaseFence)
{
    if (!buffer || width <= 0 || height <= 0)
        return Fail("sample_input", VK_ERROR_INITIALIZATION_FAILED);
    const auto found = cache_.find(OH_NativeBuffer_GetSeqNum(buffer));
    if (found == cache_.end() || found->second->native != buffer)
        return Fail("sample_not_imported", VK_ERROR_INITIALIZATION_FAILED);
    if (!context_.CommandPool() && !InitializeSampler()) return false;
    FrameSlot& slot = frameSlots_[static_cast<uint32_t>(frame) % frameSlotCount_];
    if (slot.inFlight) return Fail("sample_slot_busy", VK_ERROR_INITIALIZATION_FAILED);
    if (outputSurfaceId_ && !InitializeOutput()) return false;
    uint32_t outputImageIndex = 0;
    if (outputSurfaceId_) {
        VkResult acquired = vkAcquireNextImageKHR(context_.Device(), context_.OutputSwapchain(), 5'000'000'000ULL,
                                                    context_.OutputAcquired(static_cast<uint32_t>(frame) % frameSlotCount_),
                                                    VK_NULL_HANDLE,
                                                    &outputImageIndex);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return Fail("composite_acquire", acquired);
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler_;
    imageInfo.imageView = found->second->view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo bufferInfo{};
    if (verifyPixels_) {
        bufferInfo.buffer = slot.readback;
        bufferInfo.range = 9 * sizeof(uint32_t);
    }
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = slot.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = slot.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(context_.Device(), verifyPixels_ ? 2 : 1, writes, 0, nullptr);

    VkResult result = vkResetCommandBuffer(slot.command, 0);
    if (result != VK_SUCCESS) return Fail("sample_command_reset", result);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(slot.command, &beginInfo);
    if (result != VK_SUCCESS) return Fail("sample_command_begin", result);
    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.oldLayout = externalSharedImage_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = externalSharedImage_ ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_FOREIGN_EXT;
    imageBarrier.dstQueueFamilyIndex = context_.QueueFamily();
    imageBarrier.dstAccessMask = (verifyPixels_ || outputSurfaceId_) ?
        VK_ACCESS_SHADER_READ_BIT : 0;
    imageBarrier.image = found->second->image;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             (outputSurfaceId_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                         0, 0, nullptr, 0, nullptr,
                         1, &imageBarrier);
    if (verifyPixels_) {
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                0, 1, &slot.descriptorSet, 0, nullptr);
        const VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        vkCmdPushConstants(slot.command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(extent), &extent);
        vkCmdDispatch(slot.command, 1, 1, 1);
    }
    if (outputSurfaceId_) {
        VkClearValue clear{};
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = context_.OutputRenderPass();
        render.framebuffer = context_.OutputFramebuffer(outputImageIndex);
        render.renderArea = {{0, 0}, context_.OutputExtent()};
        render.clearValueCount = 1;
        render.pClearValues = &clear;
        vkCmdBeginRenderPass(slot.command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipeline_);
        vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                0, 1, &slot.descriptorSet, 0, nullptr);
        vkCmdDraw(slot.command, 3, 1, 0, 0);
        vkCmdEndRenderPass(slot.command);
    }

    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.newLayout = externalSharedImage_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.srcQueueFamilyIndex = context_.QueueFamily();
    imageBarrier.dstQueueFamilyIndex = externalSharedImage_ ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_FOREIGN_EXT;
    imageBarrier.srcAccessMask = (verifyPixels_ || outputSurfaceId_) ?
        VK_ACCESS_SHADER_READ_BIT : 0;
    imageBarrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             (outputSurfaceId_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0),
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &imageBarrier);
    if (verifyPixels_) {
        VkBufferMemoryBarrier readbackBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        readbackBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        readbackBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        readbackBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readbackBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readbackBarrier.buffer = slot.readback;
        readbackBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &readbackBarrier,
                             0, nullptr);
    }
    result = vkEndCommandBuffer(slot.command);
    if (result != VK_SUCCESS) return Fail("sample_command_end", result);
    result = vkResetFences(context_.Device(), 1, &slot.fence);
    if (result != VK_SUCCESS) return Fail("sample_fence_reset", result);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command;
    VkSemaphore waitSemaphores[2]{};
    VkPipelineStageFlags waitStages[2]{};
    VkSemaphore signalSemaphores[2]{};
    if (releaseFence) {
        if (!InitializeSync()) return false;
        signalSemaphores[submit.signalSemaphoreCount++] = slot.releaseSemaphore;
    }
    if (acquireFence && *acquireFence >= 0) {
        VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        importInfo.semaphore = slot.acquireSemaphore;
        importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        importInfo.fd = *acquireFence;
        result = context_.ImportSemaphoreFd()(context_.Device(), &importInfo);
        if (result != VK_SUCCESS) return Fail("fence_import", result);
        *acquireFence = -1; // Vulkan owns the fd after a successful import.
        ++acquireImports_;
        waitSemaphores[submit.waitSemaphoreCount] = slot.acquireSemaphore;
        waitStages[submit.waitSemaphoreCount++] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    if (outputSurfaceId_) {
        waitSemaphores[submit.waitSemaphoreCount] =
            context_.OutputAcquired(static_cast<uint32_t>(frame) % frameSlotCount_);
        waitStages[submit.waitSemaphoreCount++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        signalSemaphores[submit.signalSemaphoreCount++] = context_.OutputRendered(outputImageIndex);
    }
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitStages;
    submit.pSignalSemaphores = signalSemaphores;
    result = vkQueueSubmit(context_.Queue(), 1, &submit, slot.fence);
    if (result != VK_SUCCESS) return Fail("sample_submit", result);
    slot.frame = frame;
    slot.inFlight = true;
    slot.imageReference = found->second;
    if (releaseFence) {
        VkSemaphoreGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        exportInfo.semaphore = slot.releaseSemaphore;
        exportInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        result = context_.GetSemaphoreFd()(context_.Device(), &exportInfo, releaseFence);
        if (result != VK_SUCCESS) {
            // Without a release fd, finish the submitted work before the caller
            // gives the BufferQueue its buffer back.
            vkWaitForFences(context_.Device(), 1, &slot.fence, VK_TRUE, 5'000'000'000ULL);
            slot.inFlight = false;
            return Fail("fence_export", result);
        }
        ++releaseExports_;
    }
    if (outputSurfaceId_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &context_.OutputRendered(outputImageIndex);
        present.swapchainCount = 1;
        present.pSwapchains = &context_.OutputSwapchain();
        present.pImageIndices = &outputImageIndex;
        result = vkQueuePresentKHR(context_.Queue(), &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            vkWaitForFences(context_.Device(), 1, &slot.fence, VK_TRUE, 5'000'000'000ULL);
            slot.inFlight = false;
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
    if (frame < 0 || frameSlots_.empty())
        return Fail("sample_frame", VK_ERROR_INITIALIZATION_FAILED);
    FrameSlot& slot = frameSlots_[static_cast<uint32_t>(frame) % frameSlotCount_];
    if (!slot.inFlight || slot.frame != frame)
        return Fail("sample_slot_frame", VK_ERROR_INITIALIZATION_FAILED);
    VkResult result = vkWaitForFences(context_.Device(), 1, &slot.fence, VK_TRUE, 5'000'000'000ULL);
    if (result != VK_SUCCESS) return Fail("sample_wait", result);
    slot.inFlight = false;
    slot.imageReference.reset();
    if (!verifyPixels_) {
        ++samples_;
        stage_ = "frame_complete";
        error_ = VK_SUCCESS;
        return true;
    }
    void* mapped = nullptr;
    result = vkMapMemory(context_.Device(), slot.readbackMemory, 0, 9 * sizeof(uint32_t), 0, &mapped);
    if (result != VK_SUCCESS) return Fail("sample_map", result);
    const uint32_t expected = 0xffa55a00u | static_cast<uint8_t>((frame % 8) * 31 + 7);
    bool matches = true;
    for (uint32_t i = 0; i < 9; ++i) {
        if (static_cast<const uint32_t*>(mapped)[i] != expected) matches = false;
    }
    vkUnmapMemory(context_.Device(), slot.readbackMemory);
    if (!matches) return Fail("gpu_sample_mismatch", VK_ERROR_UNKNOWN);
    ++samples_;
    stage_ = "sampled";
    error_ = VK_SUCCESS;
    return true;
}

} // namespace winehua::direct
