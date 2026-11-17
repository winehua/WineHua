#include "direct_vulkan_desktop_compositor.h"
#include "direct_vulkan_context.h"
#include "direct_wine_surface_controller.h"
#include "direct_composite_spv.h"
#include "direct_desktop_spv.h"
#include "direct_frame_stats.h"
#include "common/displayed_fps.h"
#include "common/perf_utils.h"
#include "compositor/frame/shm_upload_rows.h"
#include "common/frame_loop_diagnostics.h"
#include "compositor/wayland_server.h"
#include "compositor/toplevel/desktop_compositor.h"
#include "graphics/graphics_broker.h"
#include "graphics/native_window_lease.h"
#include "graphics/display_cadence.h"
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <algorithm>
#include <array>
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

// Opt-in bounded phase totals, sharing the existing frameDiagnostics switch.
// Disabled mode never samples a clock or emits diagnostics.
class DesktopPhaseTimings {
public:
    enum Phase { Slot, Surface, Sources, Acquire, Commands, Submit, Present, Idle, Count };
    void Begin() {
        const auto state = winehua::FrameLoopDiagnosticState();
        enabled_ = (state & 1) != 0;
        if (state != state_) { sums_ = {}; counts_ = {}; state_ = state; started_ = 0; }
        if (!enabled_) return;
        last_ = winehua::PerfNowUs();
        if (!started_) started_ = last_;
        if (last_ - started_ < 2000000) return;
        OH_LOG_INFO(LOG_APP,
            "[DIRECT-TIMING] window_us=%{public}llu counts_slot_surface_sources_acquire_commands_submit_present_idle=%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu sums_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu",
            (unsigned long long)(last_ - started_),
            (unsigned long long)counts_[Slot], (unsigned long long)counts_[Surface], (unsigned long long)counts_[Sources],
            (unsigned long long)counts_[Acquire], (unsigned long long)counts_[Commands],
            (unsigned long long)counts_[Submit], (unsigned long long)counts_[Present],
            (unsigned long long)counts_[Idle],
            (unsigned long long)sums_[Slot], (unsigned long long)sums_[Surface], (unsigned long long)sums_[Sources],
            (unsigned long long)sums_[Acquire], (unsigned long long)sums_[Commands],
            (unsigned long long)sums_[Submit], (unsigned long long)sums_[Present],
            (unsigned long long)sums_[Idle]);
        sums_ = {}; counts_ = {}; started_ = last_;
    }
    void Mark(Phase phase) {
        if (!enabled_) return;
        const auto now = winehua::PerfNowUs();
        sums_[phase] += now - last_; ++counts_[phase]; last_ = now;
    }
private:
    bool enabled_ = false;
    uint64_t state_ = 0, started_ = 0, last_ = 0;
    std::array<uint64_t, Count> sums_{}, counts_{};
};

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
        // Immutable CPU snapshot matching this slot's texture. Reading mapped
        // coherent GPU staging memory can be uncached on mobile devices.
        std::shared_ptr<const std::vector<uint8_t>> uploadedPixels;
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
        DirectFrameAcceptance accepted;
        bool glSource = false;
    };
    struct Layer {
        DirectDesktopSource source;
        Buffer current;
        std::unordered_map<uint32_t, std::shared_ptr<Image>> cache;
    };
    struct GlLayer {
        uint64_t key = 0;
        ZeroCopyLayerInfo geometry;
        bool vulkan = false;
        winehua::NativeWindowLease window;
        std::shared_ptr<DirectImageQueue> queue;
        Buffer current;
        std::unordered_map<uint32_t, std::shared_ptr<Image>> cache;
        int width = 0, height = 0;
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
                for (auto& [key, layer] : glLayers_) ReleaseGlLayer(*layer);
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
        OH_LOG_INFO(LOG_APP, "retire presents=%{public}llu directDraws=%{public}llu acquireWaits=%{public}llu releaseFences=%{public}llu producerRetired=%{public}llu releaseErrors=%{public}llu directGameCpuReadBytes=0 directGameCpuUploadBytes=0",
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
        cadence_.Initialize();
        if (!SetDirectDesktopConsumer(this, true)) {
            OH_LOG_ERROR(LOG_APP, "Vulkan desktop queue ownership already held");
            return false;
        }
        desktopVulkanActive.store(true, std::memory_order_relaxed);
        OH_LOG_INFO(LOG_APP, "Vulkan desktop enabled surface=%{public}llu extent=%{public}ux%{public}u slots=2 preTransform=identity directGameCpuReadBytes=0 directGameCpuUploadBytes=0",
                    (unsigned long long)surfaceId, g.OutputExtent().width, g.OutputExtent().height);
        return true;
    }

    bool Render(int expectedW, int expectedH, bool paused, bool repaint) {
        // Visibility changes do not stop queue consumption. Drain submitted
        // reads once on background entry, then return retired buffers with
        // their original acquire fences without acquiring/presenting output.
        if (paused) {
            if (!background_) {
                for (size_t i = 0; i < 2; ++i) {
                    auto& frame = frames_[i];
                    if (frame.inFlight && !Check(vkWaitForFences(core_->Device(), 1,
                            &core_->Submission(i).fence, VK_TRUE, 2'000'000'000ULL), "background_drain")) return false;
                    frame.inFlight = false;
                    frame.references.clear();
                }
                background_ = true;
                for (const auto& [key, layer] : glLayers_)
                    GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(key, 100000000ull);
            }
            if (!UpdateSources()) return false;
            UpdateGlSources();
            // Keep the latest acquired image for a producer that stops drawing
            // while hidden (e.g. CEF). Returning it here loses the only frame
            // available on resume. UpdateSources/UpdateGlSources retire each
            // replaced image, so multi-buffer queues still advance in background
            // without copying pixels or acquiring an output image.
            return DrainBackgroundBuffers();
        }
        if (background_) {
            background_ = false;
            for (const auto& [key, layer] : glLayers_)
                GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(key, cadence_.PeriodNs());
            scene_ = {};
            renderedExtent_ = {};
        }
        PublishFrameRates();
        if (repaint) renderedExtent_ = {};
        timings_.Begin();
        auto& g = *core_;
        auto& slot = g.Submission(frameIndex_ % 2);
        auto& frame = frames_[frameIndex_ % 2];
        if (frame.inFlight) {
            const VkResult wait = vkWaitForFences(g.Device(), 1, &slot.fence, VK_TRUE, 2'000'000'000ULL);
            if (!Check(wait, "scene_slot_fence")) return false;
            frame.inFlight = false;
        }
        frame.references.clear();
        timings_.Mark(DesktopPhaseTimings::Slot);
        VkSurfaceCapabilitiesKHR capabilities{};
        if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.PhysicalDevice(), g.OutputSurface(), &capabilities), "scene_surface_caps")) return false;
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) extent = {static_cast<uint32_t>(expectedW), static_cast<uint32_t>(expectedH)};
        if (!extent.width || !extent.height) return true;
        if (extent.width != g.OutputExtent().width || extent.height != g.OutputExtent().height) {
            if (!Recreate()) return false;
        }
        timings_.Mark(DesktopPhaseTimings::Surface);
        if (!UpdateSources()) return false;
        if (cadence_.Refresh(winehua::PerfNowUs())) {
            for (const auto& [key, layer] : glLayers_)
                GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(key, cadence_.PeriodNs());
            OH_LOG_INFO(LOG_APP, "[DISPLAY-CADENCE] period_ns=%{public}llu rate=%{public}.2fHz",
                (unsigned long long)cadence_.PeriodNs(), 1e9 / cadence_.PeriodNs());
        }
        UpdateGlSources();
        std::vector<GpuDesktopDirectSource> direct;
        for (const auto& [id, layer] : layers_) if (layer.current.image)
            direct.push_back({static_cast<uint32_t>(layer.source.token.clientPid), layer.source.token.toplevelId, layer.source.token.wlSurfaceId,
                              layer.current.image->width, layer.current.image->height, layer.source.token.generation});
        GpuDesktopScene next;
        std::vector<GpuDesktopLayer> glSources;
        for (const auto& [key, layer] : glLayers_) {
            if (!layer->current.image) continue;
            const auto& info = layer->geometry;
            auto source = MakeNativeWindowLayer(key, info, layer->width, layer->height);
            // GLES framebuffer rows are bottom-up; Venus output rows are
            // already top-down. Both use the NativeWindow OPAQUE contract.
            SetNativeProducerSampling(source, layer->vulkan);
            glSources.push_back(source);
        }
        if (!compositor_.SnapshotGpuDesktopScene(direct, snapshots_, next, glSources)) {
            // Present an empty background while Wine prepares its first SHM
            // commit. XComponent startup must not depend on a game/UI frame.
            next.width = g.OutputExtent().width; next.height = g.OutputExtent().height;
        }
        bool pending = !retired_.empty();
        timings_.Mark(DesktopPhaseTimings::Sources);
        for (const auto& [id, layer] : layers_) pending |= layer.current.image && !layer.current.owned;
        for (const auto& [key, layer] : glLayers_) pending |= layer->current.image && !layer->current.owned;
        if (!pending && SameGpuDesktopScene(next, scene_) && renderedExtent_.width == g.OutputExtent().width &&
            renderedExtent_.height == g.OutputExtent().height) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            timings_.Mark(DesktopPhaseTimings::Idle);
            return true;
        }
        scene_ = std::move(next);
        uint32_t outputIndex = 0;
        VkResult result = vkAcquireNextImageKHR(g.Device(), g.OutputSwapchain(), 1'000'000'000ULL,
                                                g.OutputAcquired(frameIndex_ % 2), VK_NULL_HANDLE, &outputIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return Recreate();
        if (result == VK_TIMEOUT || result == VK_NOT_READY) return true;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return Fail("scene_acquire_output", result);
        timings_.Mark(DesktopPhaseTimings::Acquire);
        const bool suboptimal = result == VK_SUBOPTIMAL_KHR;
        if (!Check(vkResetCommandBuffer(slot.command, 0), "scene_command_reset")) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(vkBeginCommandBuffer(slot.command, &begin), "scene_command_begin")) return false;

        std::vector<VkSemaphore> waits{g.OutputAcquired(frameIndex_ % 2)};
        std::vector<VkPipelineStageFlags> waitStages{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        std::vector<Buffer*> newlyOwned;
        const auto acquireBuffer = [&](Buffer& buffer) {
            if (!buffer.image || buffer.owned) return true;
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
            return true;
        };
        for (auto& [id, layer] : layers_) if (!acquireBuffer(layer.current)) return false;
        for (auto& [key, layer] : glLayers_) if (!acquireBuffer(layer->current)) return false;
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
        std::unordered_set<uint64_t> drawnDirect;
        std::unordered_set<uint64_t> drawnGl;
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
            if (layer.directSurfaceKey) {
                auto it = layers_.find(layer.directSurfaceKey);
                if (it != layers_.end()) image = it->second.current.image;
            } else if (layer.zeroCopyKey) {
                auto it = glLayers_.find(layer.zeroCopyKey);
                if (it != glLayers_.end()) image = it->second->current.image;
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
            if (layer.directSurfaceKey) { ++draws_; drawnDirect.insert(layer.directSurfaceKey); }
            if (layer.zeroCopyKey) { ++glDraws_; drawnGl.insert(layer.zeroCopyKey); }
        }
        vkCmdEndRenderPass(slot.command);
        if (!Check(vkEndCommandBuffer(slot.command), "scene_command_end")) return false;
        if (!Check(vkResetFences(g.Device(), 1, &slot.fence), "scene_fence_reset")) return false;
        VkSemaphore signals[] = {g.OutputRendered(outputIndex), slot.releaseSemaphore};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = waits.size(); submit.pWaitSemaphores = waits.data(); submit.pWaitDstStageMask = waitStages.data();
        submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.command;
        submit.signalSemaphoreCount = retired_.empty() ? 1 : 2; submit.pSignalSemaphores = signals;
        timings_.Mark(DesktopPhaseTimings::Commands);
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
        timings_.Mark(DesktopPhaseTimings::Submit);
        result = vkQueuePresentKHR(g.Queue(), &present);
        timings_.Mark(DesktopPhaseTimings::Present);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            ++presentationSerial_;
            desktopPresents.fetch_add(1, std::memory_order_relaxed);
            bool freshGameFrame = false;
            std::unordered_set<uint32_t> freshWindows;
            for (auto& [id, layer] : layers_) {
                const bool visible = drawnDirect.count(id) != 0;
                if (layer.current.accepted.Presented(true, visible)) {
                    freshWindows.insert(layer.source.token.toplevelId);
                    freshGameFrame = true;
                }
            }
            // Several drawable queues can belong to one window. Publish one
            // accepted output frame per owning window, never a wl_surface ID.
            for (const auto top : freshWindows) windowRates_[top].Record();
            for (auto& [key, layer] : glLayers_)
                freshGameFrame |= layer->current.accepted.Presented(true, drawnGl.count(key) != 0);
            if (freshGameFrame) {
                desktopGamePresents.fetch_add(1, std::memory_order_relaxed);
                rootRate_.Record();
            } else if (direct.empty() && glSources.empty()) {
                // With no game source, display actual desktop/UI updates.
                rootRate_.Record();
            }
        }
        ++frameIndex_; ++presents_;
        renderedExtent_ = g.OutputExtent();
        if (presents_ == 1 || presents_ % 120 == 0)
            OH_LOG_INFO(LOG_APP, "scene presents=%{public}llu layers=%{public}zu direct=%{public}zu draws=%{public}llu acquireWaits=%{public}llu releaseFences=%{public}llu producerRetired=%{public}llu releaseErrors=%{public}llu imports=%{public}llu reuses=%{public}llu uiUploadBytes=%{public}llu glSources=%{public}zu glDraws=%{public}llu directGameCpuReadBytes=0 directGameCpuUploadBytes=0",
                (unsigned long long)presents_, scene_.layers.size(), direct.size(), (unsigned long long)draws_,
                (unsigned long long)acquireWaits_, (unsigned long long)releaseFences_, (unsigned long long)producerRetired_, (unsigned long long)releaseErrors_,
                (unsigned long long)imports_, (unsigned long long)reuses_, (unsigned long long)uiUploadBytes_,
                glSources.size(), (unsigned long long)glDraws_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || suboptimal) return Recreate();
        return Check(result, "scene_present");
    }
    int Width() const { return core_ ? core_->OutputExtent().width : 0; }
    int Height() const { return core_ ? core_->OutputExtent().height : 0; }
    int ContentWidth() const { return scene_.width; }
    int ContentHeight() const { return scene_.height; }
    uint64_t PresentationSerial() const { return presentationSerial_; }

private:
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
    void ReleaseGlLayer(GlLayer& layer) {
        // Withdraw the Wine fast-path marker before detaching the producer.
        // Acquired buffers retain queue ownership until their GPU release.
        compositor_.zc().Release(layer.key, compositor_.DesktopRootToplevelId());
        GraphicsBroker::GetInstance().DetachZeroCopyTarget(layer.key);
        Retire(layer.current);
        layer.cache.clear();
    }
    void UpdateGlSources() {
        auto& broker = GraphicsBroker::GetInstance();
        const uint64_t now = winehua::PerfNowUs();
        const uint32_t root = compositor_.DesktopRootToplevelId();
        if (now - lastGlQueryUs_ >= 100000) {
            lastGlQueryUs_ = now;
            std::vector<ZeroCopySurfaceInfo> sources;
            if (broker.QueryZeroCopySurfaces(sources)) {
                std::unordered_set<uint64_t> live;
                for (const auto& source : sources) {
                    if (!source.surfaceKey) continue;
                    live.insert(source.surfaceKey);
                    const auto existing = glLayers_.find(source.surfaceKey);
                    if (existing != glLayers_.end() && existing->second->vulkan != source.vulkan) {
                        // A renderer change ends this queue's sampling/cache
                        // lifetime. Wait for a fresh broker snapshot to reattach;
                        // submitted frames still retain their old queue/image.
                        ReleaseGlLayer(*existing->second);
                        glLayers_.erase(existing);
                        failedGlKeys_.erase(source.surfaceKey);
                        continue;
                    }
                    if (existing != glLayers_.end() || source.attached ||
                        failedGlKeys_.count(source.surfaceKey)) continue;
                    ZeroCopyLayerInfo geometry;
                    if (!compositor_.GetZeroCopyLayerInfo(source.surfaceKey, root,
                            source.width, source.height, geometry)) continue;
                    OH_NativeImage* consumer = OH_ConsumerSurface_Create();
                    if (!consumer) continue;
                    auto layer = std::make_unique<GlLayer>();
                    layer->key = source.surfaceKey;
                    layer->vulkan = source.vulkan;
                    layer->geometry = geometry;
                    layer->width = source.width; layer->height = source.height;
                    layer->queue = std::make_shared<DirectImageQueue>(consumer);
                    OH_ConsumerSurface_SetDefaultSize(consumer, source.width, source.height);
                    OH_ConsumerSurface_SetDefaultUsage(consumer,
                        NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
                    OH_NativeImage_SetDropBufferMode(consumer, true);
                    layer->window.Adopt(OH_NativeImage_AcquireNativeWindow(consumer),
                                       NativeWindowReleaseMode::UnreferenceNativeObject);
                    if (!layer->window || !broker.AttachZeroCopyTarget(source.surfaceKey,
                            layer->window.Get(), background_ ? 100000000ull : cadence_.PeriodNs(), source.vulkan)) continue;
                    compositor_.zc().BindSurface(source.surfaceKey, geometry.shmCommitSerial);
                    OH_LOG_INFO(LOG_APP, "[VIRGL-VK] attached key=%{public}llu size=%{public}ux%{public}u generation=%{public}llu vulkan=%{public}d",
                        (unsigned long long)source.surfaceKey, source.width, source.height,
                        (unsigned long long)geometry.bindingGeneration, source.vulkan);
                    glLayers_.emplace(source.surfaceKey, std::move(layer));
                }
                for (auto it = glLayers_.begin(); it != glLayers_.end();)
                    if (!live.count(it->first)) { ReleaseGlLayer(*it->second); it = glLayers_.erase(it); }
                    else ++it;
                for (auto it = failedGlKeys_.begin(); it != failedGlKeys_.end();)
                    if (!live.count(*it)) it = failedGlKeys_.erase(it); else ++it;
            }
        }
        for (auto it = glLayers_.begin(); it != glLayers_.end();) {
            auto& layer = *it->second;
            ZeroCopyLayerInfo geometry;
            if (!compositor_.GetZeroCopyLayerInfo(layer.key, root, layer.width, layer.height, geometry) ||
                geometry.bindingGeneration != layer.geometry.bindingGeneration) {
                ReleaseGlLayer(layer); it = glLayers_.erase(it); continue;
            }
            layer.geometry = geometry;
            OHNativeWindowBuffer* buffer = nullptr; int fence = -1;
            if (!layer.queue->Acquire(&buffer, &fence)) {
                if (fence >= 0) close(fence);
                ++it; continue;
            }
            OH_NativeWindow_NativeObjectReference(buffer);
            OH_NativeBuffer* native = nullptr;
            OH_NativeBuffer_Config config{};
            std::shared_ptr<Image> image;
            if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native) == 0 && native) {
                OH_NativeBuffer_GetConfig(native, &config);
                const uint32_t sequence = OH_NativeBuffer_GetSeqNum(native);
                auto& cached = layer.cache[sequence];
                if (!cached) { cached = Import(native, config.width, config.height); if (cached) ++imports_; }
                else ++reuses_;
                image = cached;
            }
            if (!image || !compositor_.zc().NoteLayerConsumed(layer.key, now,
                    geometry.bindingGeneration, config.width, config.height)) {
                layer.queue->ReleaseWithStatus(buffer, fence);
                OH_NativeWindow_NativeObjectUnreference(buffer);
                failedGlKeys_.insert(layer.key);
                OH_LOG_WARN(LOG_APP, "[VIRGL-VK] GPU import/identity rejected key=%{public}llu; restoring SHM",
                    (unsigned long long)layer.key);
                ReleaseGlLayer(layer); it = glLayers_.erase(it); continue;
            }
            Retire(layer.current);
            DirectDesktopSource source{}; source.queue = layer.queue;
            layer.current = {source, buffer, image, fence, false, {}, true};
            layer.current.accepted.Acquired();
            layer.width = config.width; layer.height = config.height;
            compositor_.zc().NoteProducerPresent(layer.key, now);
            compositor_.zc().Activate(layer.key, root);
            for (auto cache = layer.cache.begin(); cache != layer.cache.end();)
                if (cache->second && (cache->second->width != config.width || cache->second->height != config.height))
                    cache = layer.cache.erase(cache); else ++cache;
            for (auto cache = layer.cache.begin(); layer.cache.size() > 16 && cache != layer.cache.end();)
                if (cache->second.use_count() == 1) cache = layer.cache.erase(cache); else ++cache;
            ++it;
        }
    }
    bool DrainBackgroundBuffers() {
        const bool owned = std::any_of(retired_.begin(), retired_.end(),
            [](const Buffer& buffer) { return buffer.owned; });
        if (owned) {
            // The background-entry fence wait completes reads, but queue
            // ownership/layout still need an explicit release to FOREIGN.
            // Submit only these barriers; never acquire/present an output.
            auto& g = *core_;
            auto& slot = g.Submission(0);
            if (!Check(vkResetCommandBuffer(slot.command, 0), "background_command_reset")) return false;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (!Check(vkBeginCommandBuffer(slot.command, &begin), "background_command_begin")) return false;
            for (auto& buffer : retired_) if (buffer.owned) Barrier(slot.command, buffer, false);
            if (!Check(vkEndCommandBuffer(slot.command), "background_command_end")) return false;
            if (!Check(vkResetFences(g.Device(), 1, &slot.fence), "background_fence_reset")) return false;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.command;
            if (!Check(vkQueueSubmit(g.Queue(), 1, &submit, slot.fence), "background_release_submit")) return false;
            frames_[0].inFlight = true;
            if (!Check(vkWaitForFences(g.Device(), 1, &slot.fence, VK_TRUE,
                                      2'000'000'000ULL), "background_release_wait")) return false;
            frames_[0].inFlight = false;
        }
        for (auto& buffer : retired_) Return(buffer, -1);
        retired_.clear();
        return true;
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
        std::unordered_set<uint64_t> active;
        for (const auto& source : sources) {
            const uint64_t id = (uint64_t(uint32_t(source.token.clientPid)) << 32) | source.token.wlSurfaceId; active.insert(id);
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
            layer.current = {source, windowBuffer, image, fence, false, {}};
            layer.current.accepted.Acquired();
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
    void PublishFrameRates() {
        const uint64_t now = winehua::PerfNowUs();
        double fps = 0;
        const uint32_t root = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
        if (root != statsRoot_) { rootRate_ = {}; statsRoot_ = root; }
        if (rootRate_.Sample(now, fps) && root) {
            winehua::PublishDisplayedFpsSample(winehua::kDisplayedFpsBasePath, root,
                ++fpsSequence_, fps, now, true);
            if (winehua::FrameTraceEnabled() || (winehua::FrameLoopDiagnosticState() & 1))
                OH_LOG_INFO(LOG_APP, "[DIRECT-FPS] root=%{public}u fps=%{public}.2f directGameCpuReadBytes=0 directGameCpuUploadBytes=0", root, fps);
        }
        std::unordered_set<uint32_t> liveWindows;
        for (const auto& [key, layer] : layers_) liveWindows.insert(layer.source.token.toplevelId);
        for (auto it = windowRates_.begin(); it != windowRates_.end();)
            if (!liveWindows.count(it->first)) it = windowRates_.erase(it); else ++it;
        for (auto& [id, rate] : windowRates_)
            // The root sample belongs to the whole desktop output. Explorer
            // can also have a layer with this id; its idle rate must not
            // overwrite the fresh game rate read by the HUD.
            if (rate.Sample(now, fps) && id != root)
                winehua::PublishDisplayedFpsSample(winehua::kDisplayedFpsBasePath, id,
                    ++fpsSequence_, fps, now);
    }
    void Barrier(VkCommandBuffer command, const Buffer& buffer, bool acquire) {
        auto& g = *core_;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = buffer.image->image;
        // EGL-produced images use GENERAL across the foreign queue boundary;
        // Wine Vulkan swapchain buffers keep their PRESENT_SRC contract.
        const VkImageLayout foreign = buffer.glSource ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.oldLayout = acquire ? foreign : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : foreign;
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
        // Compare ordinary CPU snapshots, never read the GPU staging allocation.
        // Each fenced frame slot retains exactly the pixels last uploaded to its
        // own texture, even when it misses commits or alternates with another slot.
        const auto rows = winehua::PlanShmUploadRows(
            image.uploadedPixels ? image.uploadedPixels->data() : nullptr,
            layer.pixels->data(), image.width, image.height,
            image.initialized && image.uploadedPixels && image.uploadedPixels->size() == bytes);
        if (rows.bytes) {
            void* mapped = nullptr;
            // Only Wayland SHM UI is uploaded; NativeBuffer game images never map.
            if (!Check(vkMapMemory(g.Device(), image.stagingMemory, 0, bytes, 0, &mapped), "scene_ui_map")) return false;
            std::memcpy(static_cast<uint8_t*>(mapped) + rows.offset,
                layer.pixels->data() + rows.offset, rows.bytes);
            vkUnmapMemory(g.Device(), image.stagingMemory);
        }
        image.uploadedPixels = layer.pixels;
        image.serial = layer.serial;
        if (!rows.count) return true;
        uiUploadBytes_ += rows.bytes;
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
        copy.bufferOffset = rows.offset;
        copy.bufferRowLength = static_cast<uint32_t>(image.width);
        copy.imageOffset.y = static_cast<int32_t>(rows.first);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<uint32_t>(image.width), rows.count, 1};
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
    std::unordered_map<uint64_t, Layer> layers_;
    std::unordered_map<uint32_t, DirectFrameRate> windowRates_;
    std::unordered_map<uint64_t, std::unique_ptr<GlLayer>> glLayers_;
    std::unordered_set<uint64_t> failedGlKeys_;
    uint64_t lastGlQueryUs_ = 0, glDraws_ = 0;
    bool background_ = false;
    std::vector<Buffer> retired_;
    GpuDesktopSnapshotCache snapshots_;
    GpuDesktopScene scene_;
    VkExtent2D renderedExtent_{};
    uint64_t frameIndex_ = 0, presents_ = 0, draws_ = 0;
    uint64_t presentationSerial_ = 0;
    DirectFrameRate rootRate_;
    DesktopPhaseTimings timings_;
    winehua::DisplayCadence cadence_;
    uint32_t statsRoot_ = 0;
    uint64_t fpsSequence_ = 0;
    uint64_t acquireWaits_ = 0, releaseFences_ = 0, producerRetired_ = 0, releaseErrors_ = 0;
    uint64_t imports_ = 0, reuses_ = 0, uiUploadBytes_ = 0;
};

DirectVulkanDesktopCompositor::DirectVulkanDesktopCompositor(DesktopCompositor& compositor)
    : renderer_(std::make_unique<VulkanDesktopRenderer>(compositor)) {}
DirectVulkanDesktopCompositor::~DirectVulkanDesktopCompositor() = default;
bool DirectVulkanDesktopCompositor::Initialize(OHNativeWindow* window) { return renderer_->Initialize(window); }
bool DirectVulkanDesktopCompositor::Render(int width, int height, bool paused, bool repaint) { return renderer_->Render(width, height, paused, repaint); }
uint64_t DirectVulkanDesktopCompositor::PresentationSerial() const { return renderer_->PresentationSerial(); }
int DirectVulkanDesktopCompositor::Width() const { return renderer_->Width(); }
int DirectVulkanDesktopCompositor::Height() const { return renderer_->Height(); }
int DirectVulkanDesktopCompositor::ContentWidth() const { return renderer_->ContentWidth(); }
int DirectVulkanDesktopCompositor::ContentHeight() const { return renderer_->ContentHeight(); }
}
