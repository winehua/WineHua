#include "direct_vulkan_presenter.h"
#include "direct_composite_spv.h"

#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "DirectVkPresenter"

namespace winehua::direct {

DirectVulkanPresenter::~DirectVulkanPresenter()
{
    context_.Drain();
    for (auto& slot : slots_) slot.image.reset();
    cache_.clear();
    if (context_.Device()) {
        if (pipeline_) vkDestroyPipeline(context_.Device(), pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(context_.Device(), pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(context_.Device(), descriptorPool_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(context_.Device(), descriptorLayout_, nullptr);
        if (sampler_) vkDestroySampler(context_.Device(), sampler_, nullptr);
    }
}

bool DirectVulkanPresenter::Fail(const char* stage, VkResult result)
{
    stage_ = stage;
    error_ = result;
    failed_ = true; // Partial initialization/submission must be torn down by the owner.
    return false;
}

bool DirectVulkanPresenter::Initialize()
{
    if (failed_) return false;
    if (ready_) return true;
    if (!context_.Initialize() || !context_.InitializeSubmissions())
        return Fail(context_.Stage(), context_.Error());
    slots_.resize(frameSlotCount_);
    for (uint32_t i = 0; i < frameSlotCount_; ++i)
        static_cast<DirectVulkanSubmission&>(slots_[i]) = context_.Submission(i);
    if (outputSurfaceId_) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkResult result = vkCreateSampler(context_.Device(), &sampler, nullptr, &sampler_);
        if (result != VK_SUCCESS) return Fail("presenter_sampler", result);
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        result = vkCreateDescriptorSetLayout(context_.Device(), &layout, nullptr, &descriptorLayout_);
        if (result != VK_SUCCESS) return Fail("presenter_descriptor_layout", result);
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameSlotCount_};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = frameSlotCount_;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &poolSize;
        result = vkCreateDescriptorPool(context_.Device(), &pool, nullptr, &descriptorPool_);
        if (result != VK_SUCCESS) return Fail("presenter_descriptor_pool", result);
        std::vector<VkDescriptorSetLayout> layouts(frameSlotCount_, descriptorLayout_);
        std::vector<VkDescriptorSet> sets(frameSlotCount_, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = frameSlotCount_;
        allocate.pSetLayouts = layouts.data();
        result = vkAllocateDescriptorSets(context_.Device(), &allocate, sets.data());
        if (result != VK_SUCCESS) return Fail("presenter_descriptor_sets", result);
        for (uint32_t i = 0; i < frameSlotCount_; ++i) slots_[i].descriptor = sets[i];
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &descriptorLayout_;
        result = vkCreatePipelineLayout(context_.Device(), &pipelineLayout, nullptr, &pipelineLayout_);
        if (result != VK_SUCCESS) return Fail("presenter_pipeline_layout", result);
        if (!context_.InitializeOutput() || !CreatePipeline()) {
            if (failed_) return false;
            return Fail(context_.Stage(), context_.Error());
        }
    }
    ready_ = true;
    OH_LOG_INFO(LOG_APP, "Vulkan presenter enabled surface=%{public}llu extent=%{public}ux%{public}u slots=%{public}u preTransform=identity gameCpuReadBytes=0 gameCpuUploadBytes=0",
                static_cast<unsigned long long>(outputSurfaceId_), context_.OutputExtent().width,
                context_.OutputExtent().height, frameSlotCount_);
    return true;
}

bool DirectVulkanPresenter::CreatePipeline()
{
    VkShaderModule modules[2]{};
    const uint32_t* code[] = {kDirectCompositeVertSpv, kDirectCompositeFragSpv};
    const size_t sizes[] = {sizeof(kDirectCompositeVertSpv), sizeof(kDirectCompositeFragSpv)};
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = sizes[i];
        module.pCode = code[i];
        result = vkCreateShaderModule(context_.Device(), &module, nullptr, &modules[i]);
        if (result != VK_SUCCESS) {
            if (modules[0]) vkDestroyShaderModule(context_.Device(), modules[0], nullptr);
            return Fail("presenter_shader_module", result);
        }
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.f, 0.f, static_cast<float>(context_.OutputExtent().width),
                        static_cast<float>(context_.OutputExtent().height), 0.f, 1.f};
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
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportState;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.layout = pipelineLayout_;
    pipeline.renderPass = context_.OutputRenderPass();
    result = vkCreateGraphicsPipelines(context_.Device(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
    for (auto module : modules) vkDestroyShaderModule(context_.Device(), module, nullptr);
    return result == VK_SUCCESS || Fail("presenter_pipeline", result);
}

bool DirectVulkanPresenter::Retire(FrameSlot& slot)
{
    if (!slot.inFlight) return true;
    VkResult result = vkWaitForFences(context_.Device(), 1, &slot.fence, VK_TRUE, 5'000'000'000ULL);
    if (result != VK_SUCCESS) return Fail("presenter_slot_wait", result);
    slot.inFlight = false;
    slot.image.reset();
    ++completed_;
    return true;
}

bool DirectVulkanPresenter::RecreateOutput()
{
    VkResult result = context_.Drain();
    if (result != VK_SUCCESS) return Fail("presenter_output_drain", result);
    for (auto& slot : slots_) {
        if (slot.inFlight) ++completed_;
        slot.inFlight = false;
        slot.image.reset();
    }
    if (pipeline_) vkDestroyPipeline(context_.Device(), pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    if (!context_.RecreateOutput()) return Fail(context_.Stage(), context_.Error());
    if (!CreatePipeline()) return false;
    recreatePending_ = false;
    OH_LOG_INFO(LOG_APP, "output recreated extent=%{public}ux%{public}u count=%{public}u",
                context_.OutputExtent().width, context_.OutputExtent().height, context_.OutputRecreateCount());
    return true;
}

std::shared_ptr<DirectVulkanImage> DirectVulkanPresenter::Import(OH_NativeBuffer* buffer, int32_t width, int32_t height)
{
    if (inputWidth_ != width || inputHeight_ != height) {
        // In-flight slots retain old-size imports until their fences complete.
        cache_.clear();
        inputWidth_ = width;
        inputHeight_ = height;
    }
    const uint32_t sequence = OH_NativeBuffer_GetSeqNum(buffer);
    auto found = cache_.find(sequence);
    if (found != cache_.end()) {
        if (found->second->native != buffer) {
            Fail("presenter_sequence_alias", VK_ERROR_INITIALIZATION_FAILED);
            return {};
        }
        ++reuses_;
        return found->second;
    }
    auto image = std::make_shared<DirectVulkanImage>();
    if (!context_.ImportNativeBuffer(buffer, width, height, *image)) {
        Fail(context_.Stage(), context_.Error());
        return {};
    }
    // Producer swapchain rebuilds can retire buffers without changing size.
    // Bound idle imports as well; slots retain any images still used by GPU.
    for (auto entry = cache_.begin(); cache_.size() >= 16 && entry != cache_.end();) {
        if (entry->second.use_count() == 1) entry = cache_.erase(entry);
        else ++entry;
    }
    cache_.emplace(sequence, image);
    ++imports_;
    return image;
}

bool DirectVulkanPresenter::Present(OH_NativeBuffer* buffer, int32_t width, int32_t height,
                                  int* acquireFence, int* releaseFence)
{
    if (releaseFence) *releaseFence = -1;
    if (failed_) return false;
    if (!buffer || width <= 0 || height <= 0 || !acquireFence || !releaseFence)
        return Fail("presenter_input", VK_ERROR_INITIALIZATION_FAILED);
    if (!Initialize()) return false;
    if (recreatePending_ && !RecreateOutput()) return false;
    const uint32_t slotIndex = static_cast<uint32_t>(frames_ % frameSlotCount_);
    auto& slot = slots_[slotIndex];
    if (!Retire(slot)) return false;
    auto image = Import(buffer, width, height);
    if (!image) return false;
    uint32_t outputIndex = 0;
    bool suboptimal = false;
    VkResult result;
    if (outputSurfaceId_) {
        auto acquireOutput = [&] {
            return vkAcquireNextImageKHR(context_.Device(), context_.OutputSwapchain(), 5'000'000'000ULL,
                                         context_.OutputAcquired(slotIndex), VK_NULL_HANDLE, &outputIndex);
        };
        result = acquireOutput();
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!RecreateOutput()) return false;
            result = acquireOutput(); // One bounded retry before reporting failure.
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return Fail("presenter_output_acquire", result);
        suboptimal = result == VK_SUBOPTIMAL_KHR;
        VkDescriptorImageInfo imageInfo{sampler_, image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = slot.descriptor;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(context_.Device(), 1, &write, 0, nullptr);
    }
    result = vkResetCommandBuffer(slot.command, 0);
    if (result != VK_SUCCESS) return Fail("presenter_command_reset", result);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(slot.command, &begin);
    if (result != VK_SUCCESS) return Fail("presenter_command_begin", result);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = context_.QueueFamily();
    barrier.dstAccessMask = outputSurfaceId_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.image = image->image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (outputSurfaceId_) {
        VkClearValue clear{};
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = context_.OutputRenderPass();
        render.framebuffer = context_.OutputFramebuffer(outputIndex);
        render.renderArea = {{0, 0}, context_.OutputExtent()};
        render.clearValueCount = 1;
        render.pClearValues = &clear;
        vkCmdBeginRenderPass(slot.command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                0, 1, &slot.descriptor, 0, nullptr);
        vkCmdDraw(slot.command, 3, 1, 0, 0);
        vkCmdEndRenderPass(slot.command);
    }
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = context_.QueueFamily();
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.srcAccessMask = outputSurfaceId_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    result = vkEndCommandBuffer(slot.command);
    if (result != VK_SUCCESS) return Fail("presenter_command_end", result);
    result = vkResetFences(context_.Device(), 1, &slot.fence);
    if (result != VK_SUCCESS) return Fail("presenter_fence_reset", result);
    VkSemaphore waits[2]{}, signals[2]{slot.releaseSemaphore};
    VkPipelineStageFlags waitStages[2]{};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command;
    submit.signalSemaphoreCount = 1;
    if (*acquireFence >= 0) {
        // Keep the caller's fd until queue submission succeeds. If import or
        // submission fails, the unchanged fd still protects producer writes.
        const int importedFd = dup(*acquireFence);
        if (importedFd < 0) return Fail("presenter_acquire_dup", VK_ERROR_TOO_MANY_OBJECTS);
        VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        import.semaphore = slot.acquireSemaphore;
        import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        import.fd = importedFd;
        result = context_.ImportSemaphoreFd()(context_.Device(), &import);
        if (result != VK_SUCCESS) {
            close(importedFd);
            return Fail("presenter_acquire_import", result);
        }
        waits[submit.waitSemaphoreCount] = slot.acquireSemaphore;
        waitStages[submit.waitSemaphoreCount++] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        ++acquireImports_;
    }
    if (outputSurfaceId_) {
        waits[submit.waitSemaphoreCount] = context_.OutputAcquired(slotIndex);
        waitStages[submit.waitSemaphoreCount++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        signals[submit.signalSemaphoreCount++] = context_.OutputRendered(outputIndex);
    }
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = waitStages;
    submit.pSignalSemaphores = signals;
    result = vkQueueSubmit(context_.Queue(), 1, &submit, slot.fence);
    if (result != VK_SUCCESS) return Fail("presenter_submit", result);
    if (*acquireFence >= 0) { close(*acquireFence); *acquireFence = -1; }
    slot.inFlight = true;
    slot.image = std::move(image);
    ++frames_;
    VkSemaphoreGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    exportInfo.semaphore = slot.releaseSemaphore;
    exportInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    result = context_.GetSemaphoreFd()(context_.Device(), &exportInfo, releaseFence);
    if (result != VK_SUCCESS) return Fail("presenter_release_export", result);
    ++releaseExports_;
    if (outputSurfaceId_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &context_.OutputRendered(outputIndex);
        present.swapchainCount = 1;
        present.pSwapchains = &context_.OutputSwapchain();
        present.pImageIndices = &outputIndex;
        result = vkQueuePresentKHR(context_.Queue(), &present);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ++outputPresents_;
        else if (result != VK_ERROR_OUT_OF_DATE_KHR) return Fail("presenter_output_present", result);
        recreatePending_ = suboptimal || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR;
    }
    if (frames_ == 1 || frames_ % 60 == 0)
        OH_LOG_INFO(LOG_APP, "frames=%{public}llu completed=%{public}llu presents=%{public}llu imports=%{public}llu reuses=%{public}llu cache=%{public}zu acquireImports=%{public}llu releaseExports=%{public}llu outputRecreates=%{public}u",
                    static_cast<unsigned long long>(frames_), static_cast<unsigned long long>(completed_),
                    static_cast<unsigned long long>(outputPresents_), static_cast<unsigned long long>(imports_),
                    static_cast<unsigned long long>(reuses_), cache_.size(),
                    static_cast<unsigned long long>(acquireImports_), static_cast<unsigned long long>(releaseExports_),
                    context_.OutputRecreateCount());
    stage_ = "presenter_submitted";
    error_ = VK_SUCCESS;
    return true;
}

} // namespace winehua::direct
