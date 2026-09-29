#include "direct_vulkan_desktop_compositor.h"
#include "direct_vulkan_context.h"
#include "direct_wine_surface_controller.h"
#include "direct_composite_spv.h"
#include "direct_desktop_spv.h"
#include "compositor/toplevel/desktop_compositor.h"
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <unordered_set>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "DirectVkDesktop"

namespace winehua::direct {

static std::atomic<bool> desktopVulkanEnabled{false};
void SetDirectDesktopVulkanEnabled(bool enabled) { desktopVulkanEnabled.store(enabled); }
bool DirectDesktopVulkanEnabled() { return desktopVulkanEnabled.load(); }
static std::atomic<bool> desktopVulkanActive{false};
static std::atomic<uint64_t> desktopPresents{0}, desktopGamePresents{0};
DirectDesktopPerformance GetDirectDesktopPerformance() {
    return {desktopVulkanActive.load(std::memory_order_relaxed),
            desktopPresents.load(std::memory_order_relaxed), desktopGamePresents.load(std::memory_order_relaxed)};
}

class VulkanDesktopRenderer {
    struct Image : DirectVulkanImage {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        uint64_t serial = UINT64_MAX;
        bool initialized = false;
        ~Image() {
            if (descriptor) vkFreeDescriptorSets(device, pool, 1, &descriptor);
            if (staging) vkDestroyBuffer(device, staging, nullptr);
            if (stagingMemory) vkFreeMemory(device, stagingMemory, nullptr);
        }
    };
    struct Buffer {
        DirectDesktopSource source;
        OHNativeWindowBuffer* windowBuffer = nullptr;
        std::shared_ptr<Image> image;
        int acquireFence = -1;
        bool owned = false;
    };
    struct Layer {
        DirectDesktopSource source;
        Buffer current;
        std::unordered_map<uint32_t, std::shared_ptr<Image>> cache;
    };
    struct Frame {
        bool inFlight = false;
        std::vector<VkSemaphore> waits;
        std::vector<std::shared_ptr<Image>> references;
        std::unordered_map<uint64_t, std::shared_ptr<Image>> ui;
    };

public:
    explicit VulkanDesktopRenderer(DesktopCompositor& compositor) : compositor_(compositor) {}
    ~VulkanDesktopRenderer() {
        desktopVulkanActive.store(false, std::memory_order_relaxed);
        if (core_) {
            auto& g = *core_;
            if (g.Device()) {
                vkDeviceWaitIdle(g.Device());
                // Finish queue ownership transfers before another consumer can
                // take over. No ordinary frame calls device/queue WaitIdle.
                for (auto& [id, layer] : layers_) Retire(layer.current);
                if (!retired_.empty() && g.CommandPool()) {
                    auto& slot = g.Submission(0);
                    vkResetCommandBuffer(slot.command, 0);
                    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    if (vkBeginCommandBuffer(slot.command, &begin) == VK_SUCCESS) {
                        for (auto& buffer : retired_) if (buffer.owned) Barrier(slot.command, buffer, false);
                        if (vkEndCommandBuffer(slot.command) == VK_SUCCESS) {
                            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                            submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.command;
                            if (vkQueueSubmit(g.Queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS)
                                vkQueueWaitIdle(g.Queue());
                        }
                    }
                }
                for (auto& buffer : retired_) Return(buffer, -1);
                retired_.clear(); layers_.clear();
                for (auto& frame : frames_) {
                    frame.references.clear(); frame.ui.clear();
                    for (auto sem : frame.waits) vkDestroySemaphore(g.Device(), sem, nullptr);
                }
                if (pipeline_) vkDestroyPipeline(g.Device(), pipeline_, nullptr);
                if (pipelineLayout_) vkDestroyPipelineLayout(g.Device(), pipelineLayout_, nullptr);
                if (pool_) vkDestroyDescriptorPool(g.Device(), pool_, nullptr);
                if (descriptorLayout_) vkDestroyDescriptorSetLayout(g.Device(), descriptorLayout_, nullptr);
                if (sampler_) vkDestroySampler(g.Device(), sampler_, nullptr);
            }
        }
        SetDirectDesktopConsumer(this, false);
        compositor_.ClearDirectDesktopContentSizes();
        OH_LOG_INFO(LOG_APP, "retire presents=%{public}llu directDraws=%{public}llu acquireWaits=%{public}llu releaseFences=%{public}llu producerRetired=%{public}llu releaseErrors=%{public}llu gameCpuReadBytes=0 gameCpuUploadBytes=0",
            (unsigned long long)presents_, (unsigned long long)draws_, (unsigned long long)acquireWaits_,
            (unsigned long long)releaseFences_, (unsigned long long)producerRetired_, (unsigned long long)releaseErrors_);
    }

    bool Initialize(OHNativeWindow* window) {
        uint64_t surfaceId = 0;
        if (OH_NativeWindow_GetSurfaceId(window, &surfaceId) != 0 || !surfaceId) return false;
        core_ = std::make_unique<DirectVulkanContext>(DirectVulkanConfig{surfaceId, 2, true, true});
        auto& g = *core_;
        if (!g.Initialize() || !g.InitializeSubmissions() || !g.InitializeOutput() || !InitializeSampler())
            return Fail(g.Stage(), static_cast<VkResult>(g.Error()));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool.maxSets = size.descriptorCount; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        if (!Check(vkCreateDescriptorPool(g.Device(), &pool, nullptr, &pool_), "scene_descriptor_pool")) return false;
        if (!CreatePipeline()) return false;
        SetDirectDesktopConsumer(this, true);
        desktopVulkanActive.store(true, std::memory_order_relaxed);
        OH_LOG_INFO(LOG_APP, "Vulkan desktop enabled surface=%{public}llu extent=%{public}ux%{public}u slots=2 preTransform=identity gameCpuReadBytes=0 gameCpuUploadBytes=0",
                    (unsigned long long)surfaceId, g.OutputExtent().width, g.OutputExtent().height);
        return true;
    }

    bool Render(int expectedW, int expectedH) {
        auto& g = *core_;
        auto& slot = g.Submission(frameIndex_ % 2);
        auto& frame = frames_[frameIndex_ % 2];
        if (frame.inFlight) {
            const VkResult wait = vkWaitForFences(g.Device(), 1, &slot.fence, VK_TRUE, 2'000'000'000ULL);
            if (!Check(wait, "scene_slot_fence")) return false;
            frame.inFlight = false;
        }
        frame.references.clear();
        VkSurfaceCapabilitiesKHR capabilities{};
        if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.PhysicalDevice(), g.OutputSurface(), &capabilities), "scene_surface_caps")) return false;
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) extent = {static_cast<uint32_t>(expectedW), static_cast<uint32_t>(expectedH)};
        if (!extent.width || !extent.height) return true;
        if (extent.width != g.OutputExtent().width || extent.height != g.OutputExtent().height) {
            if (!Recreate()) return false;
        }
        if (!UpdateSources()) return false;
        std::vector<GpuDesktopDirectSource> direct;
        for (const auto& [id, layer] : layers_) if (layer.current.image)
            direct.push_back({static_cast<uint32_t>(layer.source.token.clientPid), id, layer.source.token.wlSurfaceId,
                              layer.current.image->width, layer.current.image->height});
        GpuDesktopScene next;
        if (!compositor_.SnapshotGpuDesktopScene(direct, snapshots_, next)) {
            // Present an empty background while Wine prepares its first SHM
            // commit. XComponent startup must not depend on a game/UI frame.
            next.width = g.OutputExtent().width; next.height = g.OutputExtent().height;
        }
        bool pending = !retired_.empty();
        for (const auto& [id, layer] : layers_) pending |= layer.current.image && !layer.current.owned;
        if (!pending && SameScene(next, scene_) && renderedExtent_.width == g.OutputExtent().width &&
            renderedExtent_.height == g.OutputExtent().height) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            return true;
        }
        scene_ = std::move(next);
        uint32_t outputIndex = 0;
        VkResult result = vkAcquireNextImageKHR(g.Device(), g.OutputSwapchain(), 1'000'000'000ULL,
                                                g.OutputAcquired(frameIndex_ % 2), VK_NULL_HANDLE, &outputIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return Recreate();
        if (result == VK_TIMEOUT || result == VK_NOT_READY) return true;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return Fail("scene_acquire_output", result);
        const bool suboptimal = result == VK_SUBOPTIMAL_KHR;
        if (!Check(vkResetCommandBuffer(slot.command, 0), "scene_command_reset")) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(vkBeginCommandBuffer(slot.command, &begin), "scene_command_begin")) return false;

        std::vector<VkSemaphore> waits{g.OutputAcquired(frameIndex_ % 2)};
        std::vector<VkPipelineStageFlags> waitStages{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        std::vector<Buffer*> newlyOwned;
        for (auto& [id, layer] : layers_) {
            auto& buffer = layer.current;
            if (!buffer.image || buffer.owned) continue;
            if (buffer.acquireFence >= 0) {
                size_t index = waits.size() - 1;
                if (frame.waits.size() <= index) {
                    VkSemaphore sem = VK_NULL_HANDLE;
                    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                    if (!Check(vkCreateSemaphore(g.Device(), &info, nullptr, &sem), "scene_acquire_semaphore")) return false;
                    frame.waits.push_back(sem);
                }
                const int backup = dup(buffer.acquireFence);
                if (backup < 0) return Fail("scene_acquire_fd_dup", VK_ERROR_INITIALIZATION_FAILED);
                VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
                import.semaphore = frame.waits[index];
                import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
                import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                import.fd = buffer.acquireFence;
                result = g.ImportSemaphoreFd()(g.Device(), &import);
                if (result != VK_SUCCESS) { close(backup); return Fail("scene_acquire_fd_import", result); }
                buffer.acquireFence = backup; // error cleanup preserves producer readiness
                waits.push_back(import.semaphore);
                waitStages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                ++acquireWaits_;
            }
            Barrier(slot.command, buffer, true);
            newlyOwned.push_back(&buffer);
        }
        for (auto& buffer : retired_) {
            if (buffer.owned) Barrier(slot.command, buffer, false);
            frame.references.push_back(buffer.image);
        }

        std::unordered_set<uint64_t> usedUi;
        for (const auto& layer : scene_.layers) if (layer.pixels) {
            usedUi.insert(layer.key);
            auto& image = frame.ui[layer.key];
            if (!image || image->width != layer.sourceW || image->height != layer.sourceH)
                image = CreateUiImage(layer.sourceW, layer.sourceH);
            if (!image || !UploadUi(slot.command, *image, layer)) return false;
        }
        for (auto it = frame.ui.begin(); it != frame.ui.end();)
            if (!usedUi.count(it->first)) it = frame.ui.erase(it); else ++it;

        VkClearValue clear{}; clear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = g.OutputRenderPass();
        render.framebuffer = g.OutputFramebuffer(outputIndex);
        render.renderArea = {{0, 0}, g.OutputExtent()};
        render.clearValueCount = 1; render.pClearValues = &clear;
        vkCmdBeginRenderPass(slot.command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        FitRect fit;
        ComputeFitRect(g.OutputExtent().width, g.OutputExtent().height, scene_.width, scene_.height, fit);
        uint64_t frameGameDraws = 0;
        for (const auto& layer : scene_.layers) {
            if (layer.solidBlack) {
                VkClearAttachment attachment{};
                attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                attachment.clearValue.color.float32[3] = 1.0f;
                VkClearRect rectangle{{{fit.offX, fit.offY}, {static_cast<uint32_t>(fit.dstW), static_cast<uint32_t>(fit.dstH)}}, 0, 1};
                vkCmdClearAttachments(slot.command, 1, &attachment, 1, &rectangle);
                continue;
            }
            std::shared_ptr<Image> image;
            if (layer.directToplevel) {
                auto it = layers_.find(layer.directToplevel);
                if (it != layers_.end()) image = it->second.current.image;
            } else if (layer.pixels) image = frame.ui.at(layer.key);
            if (!image) continue;
            const int x = FitMapDisplayX(fit, layer.x), y = FitMapDisplayY(fit, layer.y);
            const int w = FitSizeDisplayW(fit, layer.w), h = FitSizeDisplayH(fit, layer.h);
            const int left = std::max(0, x), top = std::max(0, y);
            const int right = std::min(static_cast<int>(g.OutputExtent().width), x + w);
            const int bottom = std::min(static_cast<int>(g.OutputExtent().height), y + h);
            if (w <= 0 || h <= 0 || right <= left || bottom <= top) continue;
            VkViewport viewport{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
            VkRect2D scissor{{left, top}, {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)}};
            vkCmdSetViewport(slot.command, 0, 1, &viewport);
            vkCmdSetScissor(slot.command, 0, 1, &scissor);
            vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &image->descriptor, 0, nullptr);
            auto sampling = layer.sampling;
            sampling.u[3] = layer.opaque ? 1.0f : 0.0f;
            vkCmdPushConstants(slot.command, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sampling), &sampling);
            vkCmdDraw(slot.command, 3, 1, 0, 0);
            frame.references.push_back(image);
            if (layer.directToplevel) { ++draws_; ++frameGameDraws; }
        }
        vkCmdEndRenderPass(slot.command);
        if (!Check(vkEndCommandBuffer(slot.command), "scene_command_end")) return false;
        if (!Check(vkResetFences(g.Device(), 1, &slot.fence), "scene_fence_reset")) return false;
        VkSemaphore signals[] = {g.OutputRendered(outputIndex), slot.releaseSemaphore};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = waits.size(); submit.pWaitSemaphores = waits.data(); submit.pWaitDstStageMask = waitStages.data();
        submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.command;
        submit.signalSemaphoreCount = retired_.empty() ? 1 : 2; submit.pSignalSemaphores = signals;
        if (!Check(vkQueueSubmit(g.Queue(), 1, &submit, slot.fence), "scene_submit")) return false;
        frame.inFlight = true;
        for (auto* buffer : newlyOwned) {
            buffer->owned = true;
            if (buffer->acquireFence >= 0) close(buffer->acquireFence);
            buffer->acquireFence = -1;
        }
        if (!retired_.empty()) {
            int fd = -1;
            VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
            get.semaphore = slot.releaseSemaphore;
            get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            result = g.GetSemaphoreFd()(g.Device(), &get, &fd);
            if (result != VK_SUCCESS) {
                OH_LOG_ERROR(LOG_APP, "release fence export vk=%{public}d; draining for exceptional cleanup", result);
                if (!Check(vkDeviceWaitIdle(g.Device()), "scene_release_cleanup_idle")) return false;
            }
            for (auto& buffer : retired_) {
                int copy = fd >= 0 ? dup(fd) : -1;
                if (fd >= 0 && copy < 0 && !Check(vkWaitForFences(g.Device(), 1, &slot.fence, VK_TRUE, 2'000'000'000ULL), "scene_release_dup_cleanup")) return false;
                Return(buffer, copy);
            }
            if (fd >= 0) close(fd);
            retired_.clear();
        }
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &g.OutputRendered(outputIndex);
        present.swapchainCount = 1; present.pSwapchains = &g.OutputSwapchain(); present.pImageIndices = &outputIndex;
        result = vkQueuePresentKHR(g.Queue(), &present);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            desktopPresents.fetch_add(1, std::memory_order_relaxed);
            if (frameGameDraws) desktopGamePresents.fetch_add(1, std::memory_order_relaxed);
        }
        ++frameIndex_; ++presents_;
        renderedExtent_ = g.OutputExtent();
        if (presents_ == 1 || presents_ % 120 == 0)
            OH_LOG_INFO(LOG_APP, "scene presents=%{public}llu layers=%{public}zu direct=%{public}zu draws=%{public}llu acquireWaits=%{public}llu releaseFences=%{public}llu producerRetired=%{public}llu releaseErrors=%{public}llu imports=%{public}llu reuses=%{public}llu uiUploadBytes=%{public}llu gameCpuReadBytes=0 gameCpuUploadBytes=0",
                (unsigned long long)presents_, scene_.layers.size(), direct.size(), (unsigned long long)draws_,
                (unsigned long long)acquireWaits_, (unsigned long long)releaseFences_, (unsigned long long)producerRetired_, (unsigned long long)releaseErrors_,
                (unsigned long long)imports_, (unsigned long long)reuses_, (unsigned long long)uiUploadBytes_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || suboptimal) return Recreate();
        return Check(result, "scene_present");
    }
    int Width() const { return core_ ? core_->OutputExtent().width : 0; }
    int Height() const { return core_ ? core_->OutputExtent().height : 0; }
    int ContentWidth() const { return scene_.width; }
    int ContentHeight() const { return scene_.height; }

private:
    static bool SameScene(const GpuDesktopScene& a, const GpuDesktopScene& b) {
        if (a.rootId != b.rootId || a.width != b.width || a.height != b.height || a.layers.size() != b.layers.size()) return false;
        for (size_t i = 0; i < a.layers.size(); ++i) {
            const auto& x = a.layers[i]; const auto& y = b.layers[i];
            if (x.key != y.key || x.serial != y.serial || x.pixels != y.pixels || x.directToplevel != y.directToplevel ||
                x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h || x.opaque != y.opaque ||
                x.sourceW != y.sourceW || x.sourceH != y.sourceH || x.sampling != y.sampling ||
                x.solidBlack != y.solidBlack) return false;
        }
        return true;
    }
    bool Fail(const char* stage, VkResult result) {
        OH_LOG_ERROR(LOG_APP, "stage=%{public}s vk=%{public}d", stage, result); return false;
    }
    bool Check(VkResult result, const char* stage) { return result == VK_SUCCESS || Fail(stage, result); }
    bool Recreate() {
        auto& g = *core_;
        if (!Check(vkDeviceWaitIdle(g.Device()), "scene_resize_idle")) return false;
        if (pipeline_) vkDestroyPipeline(g.Device(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        for (auto& frame : frames_) frame.inFlight = false;
        if (!g.RecreateOutput()) return Fail(g.Stage(), static_cast<VkResult>(g.Error()));
        if (!CreatePipeline()) return false;
        OH_LOG_INFO(LOG_APP, "output recreated extent=%{public}ux%{public}u", g.OutputExtent().width, g.OutputExtent().height);
        return true;
    }
    void Retire(Buffer& buffer) {
        if (buffer.windowBuffer) retired_.push_back(std::move(buffer));
        buffer = {};
    }
    void Return(Buffer& buffer, int releaseFd) {
        if (!buffer.windowBuffer) { if (releaseFd >= 0) close(releaseFd); return; }
        if (!buffer.owned && buffer.acquireFence >= 0) {
            if (releaseFd >= 0) close(releaseFd);
            releaseFd = buffer.acquireFence; buffer.acquireFence = -1;
        }
        const auto status = buffer.source.queue->ReleaseWithStatus(buffer.windowBuffer, releaseFd);
        if (status == DirectImageQueue::ReleaseStatus::RetiredByProducer) ++producerRetired_;
        else if (status == DirectImageQueue::ReleaseStatus::Error) ++releaseErrors_;
        else if (releaseFd >= 0) ++releaseFences_;
        OH_NativeWindow_NativeObjectUnreference(buffer.windowBuffer);
        buffer.windowBuffer = nullptr;
    }
    bool UpdateSources() {
        auto sources = GetDirectDesktopSources(this);
        std::unordered_set<uint32_t> active;
        for (const auto& source : sources) {
            uint32_t id = source.token.toplevelId; active.insert(id);
            auto& layer = layers_[id];
            if (layer.source.queue != source.queue || layer.source.token.generation != source.token.generation) {
                Retire(layer.current); layer.cache.clear();
            }
            layer.source = source;
            OHNativeWindowBuffer* windowBuffer = nullptr; int fence = -1;
            if (!source.queue->Acquire(&windowBuffer, &fence)) { if (fence >= 0) close(fence); continue; }
            OH_NativeWindow_NativeObjectReference(windowBuffer);
            OH_NativeBuffer* native = nullptr; OH_NativeBuffer_Config config{};
            if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &native) != 0 || !native) {
                source.queue->ReleaseWithStatus(windowBuffer, fence);
                OH_NativeWindow_NativeObjectUnreference(windowBuffer);
                return Fail("scene_native_buffer", VK_ERROR_INITIALIZATION_FAILED);
            }
            OH_NativeBuffer_GetConfig(native, &config);
            const uint32_t sequence = OH_NativeBuffer_GetSeqNum(native);
            auto& image = layer.cache[sequence];
            if (!image) { image = Import(native, config.width, config.height); if (image) ++imports_; }
            else ++reuses_;
            if (!image) {
                source.queue->ReleaseWithStatus(windowBuffer, fence);
                OH_NativeWindow_NativeObjectUnreference(windowBuffer);
                return false;
            }
            Retire(layer.current);
            layer.current = {source, windowBuffer, image, fence, false};
            for (auto it = layer.cache.begin(); it != layer.cache.end();)
                if (it->second && (it->second->width != config.width || it->second->height != config.height))
                    it = layer.cache.erase(it); else ++it;
            // A producer may rebuild an equal-size swapchain. Bound stale
            // sequence retention without destroying images still in flight.
            for (auto it = layer.cache.begin(); layer.cache.size() > 16 && it != layer.cache.end();)
                if (it->second.use_count() == 1) it = layer.cache.erase(it); else ++it;
        }
        for (auto it = layers_.begin(); it != layers_.end();)
            if (!active.count(it->first)) { Retire(it->second.current); it = layers_.erase(it); } else ++it;
        return true;
    }
    void Barrier(VkCommandBuffer command, const Buffer& buffer, bool acquire) {
        auto& g = *core_;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = buffer.image->image;
        barrier.oldLayout = acquire ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : g.QueueFamily();
        barrier.dstQueueFamilyIndex = acquire ? g.QueueFamily() : VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    bool Describe(Image& image, VkFormat format) {
        auto& g = *core_;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!image.view && !Check(vkCreateImageView(g.Device(), &view, nullptr, &image.view), "scene_image_view")) return false;
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set.descriptorPool = pool_; set.descriptorSetCount = 1; set.pSetLayouts = &descriptorLayout_;
        if (!Check(vkAllocateDescriptorSets(g.Device(), &set, &image.descriptor), "scene_image_descriptor")) return false;
        VkDescriptorImageInfo info{sampler_, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = image.descriptor; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
        vkUpdateDescriptorSets(g.Device(), 1, &write, 0, nullptr); return true;
    }
    std::shared_ptr<Image> Import(OH_NativeBuffer* native, int width, int height) {
        auto& g = *core_;
        auto image = std::make_shared<Image>();
        image->pool = pool_;
        if (!g.ImportNativeBuffer(native, width, height, *image)) {
            Fail(g.Stage(), g.Error()); return nullptr;
        }
        if (!Describe(*image, image->format)) return nullptr;
        return image;
    }
    bool Allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) {
        auto& g = *core_;
        VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(g.PhysicalDevice(), &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = i;
                return Check(vkAllocateMemory(g.Device(), &allocation, nullptr, &memory), "scene_ui_memory");
            }
        return Fail("scene_ui_memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    std::shared_ptr<Image> CreateUiImage(int width, int height) {
        if (width <= 0 || height <= 0) return nullptr;
        auto& g = *core_;
        auto image = std::make_shared<Image>();
        image->device = g.Device(); image->pool = pool_; image->width = width; image->height = height;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_B8G8R8A8_UNORM;
        info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!Check(vkCreateImage(g.Device(), &info, nullptr, &image->image), "scene_ui_image")) return nullptr;
        VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(g.Device(), image->image, &requirements);
        if (!Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image->memory) ||
            !Check(vkBindImageMemory(g.Device(), image->image, image->memory, 0), "scene_ui_bind") ||
            !Describe(*image, info.format)) return nullptr;
        VkBufferCreateInfo staging{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        staging.size = static_cast<VkDeviceSize>(width) * height * 4; staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateBuffer(g.Device(), &staging, nullptr, &image->staging), "scene_ui_staging")) return nullptr;
        vkGetBufferMemoryRequirements(g.Device(), image->staging, &requirements);
        if (!Allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, image->stagingMemory) ||
            !Check(vkBindBufferMemory(g.Device(), image->staging, image->stagingMemory, 0), "scene_ui_staging_bind")) return nullptr;
        return image;
    }
    bool UploadUi(VkCommandBuffer command, Image& image, const GpuDesktopLayer& layer) {
        if (image.initialized && image.serial == layer.serial) return true;
        auto& g = *core_;
        const size_t bytes = static_cast<size_t>(image.width) * image.height * 4;
        void* mapped = nullptr;
        // Only Wayland SHM UI is uploaded; NativeBuffer game images never map.
        if (!Check(vkMapMemory(g.Device(), image.stagingMemory, 0, bytes, 0, &mapped), "scene_ui_map")) return false;
        std::memcpy(mapped, layer.pixels->data(), bytes); vkUnmapMemory(g.Device(), image.stagingMemory);
        uiUploadBytes_ += bytes;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = image.image;
        barrier.oldLayout = image.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask = image.initialized ? VK_ACCESS_SHADER_READ_BIT : 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, image.initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height), 1};
        vkCmdCopyBufferToImage(command, image.staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.initialized = true; image.serial = layer.serial; return true;
    }
    bool InitializeSampler() {
        auto& g = *core_;
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(vkCreateSampler(g.Device(), &sampler, nullptr, &sampler_), "scene_sampler")) return false;
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1; layout.pBindings = &binding;
        return Check(vkCreateDescriptorSetLayout(g.Device(), &layout, nullptr, &descriptorLayout_), "scene_descriptor_layout");
    }
    bool CreatePipeline() {
        auto& g = *core_;
        if (!pipelineLayout_) {
            VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DirectImageSampling)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount = 1; layout.pSetLayouts = &descriptorLayout_;
            layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
            if (!Check(vkCreatePipelineLayout(g.Device(), &layout, nullptr, &pipelineLayout_), "scene_pipeline_layout")) return false;
        }
        VkShaderModule modules[2]{};
        const uint32_t* code[] = {kDirectCompositeVertSpv, kDirectDesktopFragSpv};
        const size_t sizes[] = {sizeof(kDirectCompositeVertSpv), sizeof(kDirectDesktopFragSpv)};
        for (uint32_t i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = sizes[i]; info.pCode = code[i];
            if (!Check(vkCreateShaderModule(g.Device(), &info, nullptr, &modules[i]), "scene_shader")) {
                if (modules[0]) vkDestroyShaderModule(g.Device(), modules[0], nullptr); return false;
            }
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = modules[i]; stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD; attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
        const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &samples;
        info.pColorBlendState = &blend; info.pDynamicState = &dynamic; info.layout = pipelineLayout_; info.renderPass = g.OutputRenderPass();
        VkResult result = vkCreateGraphicsPipelines(g.Device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
        for (auto module : modules) vkDestroyShaderModule(g.Device(), module, nullptr);
        return Check(result, "scene_pipeline");
    }

    DesktopCompositor& compositor_;
    std::unique_ptr<DirectVulkanContext> core_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Frame frames_[2];
    std::unordered_map<uint32_t, Layer> layers_;
    std::vector<Buffer> retired_;
    GpuDesktopSnapshotCache snapshots_;
    GpuDesktopScene scene_;
    VkExtent2D renderedExtent_{};
    uint64_t frameIndex_ = 0, presents_ = 0, draws_ = 0;
    uint64_t acquireWaits_ = 0, releaseFences_ = 0, producerRetired_ = 0, releaseErrors_ = 0;
    uint64_t imports_ = 0, reuses_ = 0, uiUploadBytes_ = 0;
};

DirectVulkanDesktopCompositor::DirectVulkanDesktopCompositor(DesktopCompositor& compositor)
    : renderer_(std::make_unique<VulkanDesktopRenderer>(compositor)) {}
DirectVulkanDesktopCompositor::~DirectVulkanDesktopCompositor() = default;
bool DirectVulkanDesktopCompositor::Initialize(OHNativeWindow* window) { return renderer_->Initialize(window); }
bool DirectVulkanDesktopCompositor::Render(int width, int height) { return renderer_->Render(width, height); }
int DirectVulkanDesktopCompositor::Width() const { return renderer_->Width(); }
int DirectVulkanDesktopCompositor::Height() const { return renderer_->Height(); }
int DirectVulkanDesktopCompositor::ContentWidth() const { return renderer_->ContentWidth(); }
int DirectVulkanDesktopCompositor::ContentHeight() const { return renderer_->ContentHeight(); }
}
