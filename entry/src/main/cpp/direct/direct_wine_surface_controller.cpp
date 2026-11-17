#include "direct_wine_surface_controller.h"
#include "direct_vulkan_presenter.h"
#include "direct_consumer_scope.h"
#include "proc/wine_child_ipc_launcher.h"

#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "DirectWineSurface"

namespace {

using winehua::wineipc::DirectSurfaceToken;
using winehua::direct::DirectImageQueue;

struct Slot {
    uint32_t pid = 0;
    uint32_t toplevelId = 0;
    uint32_t wlSurfaceId = 0;
    int32_t desiredWidth = 64;
    int32_t desiredHeight = 64;
    uint64_t revision = 1;
    uint64_t generation = 0;
    bool active = true;
    bool desktopDrawable = false;
    std::shared_ptr<DirectImageQueue> consumer;
    const void* readyOwner = nullptr;
    DirectSurfaceToken attached{};
    uint64_t outputSurfaceId = 0;
    uint64_t outputRevision = 0;
    uint64_t outputAckRevision = 0;
    int32_t outputWidth = 0;
    int32_t outputHeight = 0;
};

struct ConsumerState {
    uint32_t top = 0;
    std::unique_ptr<winehua::direct::DirectVulkanPresenter> presenter;
    uint64_t frames = 0;
    uint64_t outputSurfaceId = 0;
    uint64_t outputRevision = 0;
};

class Controller {
public:
    Controller() { std::thread(&Controller::Run, this).detach(); }

    uint64_t Create(uint32_t pid, uint32_t toplevelId, uint32_t wlSurfaceId,
                    int32_t width = 64, int32_t height = 64, bool drawable = false)
    {
        if (!pid || !toplevelId || !wlSurfaceId || width <= 0 || height <= 0) return 0;
        const bool directReady = WineIpcChildUsesDirectVulkan(static_cast<int32_t>(pid));
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t key = (uint64_t(pid) << 32) | wlSurfaceId;
        Slot& slot = slots_[key];
        if (slot.active && slot.pid == pid && slot.wlSurfaceId == wlSurfaceId &&
            slot.toplevelId == toplevelId) {
            if (slot.desiredWidth != width || slot.desiredHeight != height) {
                slot.desiredWidth = width; slot.desiredHeight = height;
                ++slot.revision; EnqueueLocked(key);
            }
            return slot.generation;
        }
        // Toplevel IDs are allocated monotonically by the compositor. A
        // replacement must first detach its previous queue on the worker.
        slot.desktopDrawable = drawable;
        slot.pid = pid;
        slot.toplevelId = toplevelId;
        slot.wlSurfaceId = wlSurfaceId;
        slot.desiredWidth = width;
        slot.desiredHeight = height;
        slot.generation = nextGeneration_.fetch_add(1, std::memory_order_relaxed);
        slot.active = true;
        slot.outputSurfaceId = 0;
        ++slot.outputRevision;
        slot.outputWidth = 0;
        slot.outputHeight = 0;
        boundOutputs_.erase(toplevelId);
        ++slot.revision;
        if (directReady) EnqueueLocked(key);
        return slot.generation;
    }

    void ChildReady(uint32_t pid)
    {
        if (!WineIpcChildUsesDirectVulkan(static_cast<int32_t>(pid))) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, slot] : slots_)
            if (slot.active && slot.pid == pid) EnqueueLocked(id);
    }

    void Resize(uint32_t toplevelId, int32_t width, int32_t height)
    {
        if (!toplevelId || width <= 0 || height <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = FindToplevelLocked(toplevelId);
        if (it == slots_.end() || !it->second.active) return;
        Slot& slot = it->second;
        if (slot.desiredWidth == width && slot.desiredHeight == height &&
            slot.attached.width == width && slot.attached.height == height)
            return;
        slot.desiredWidth = width;
        slot.desiredHeight = height;
        ++slot.revision;
        EnqueueLocked(it->first);
    }

    void Destroy(uint32_t toplevelId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, slot] : slots_) if (slot.toplevelId == toplevelId) {
            slot.active = false; ++slot.revision; EnqueueLocked(key);
        }
    }

    void DestroyDrawable(uint32_t pid, uint32_t surface)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t key = (uint64_t(pid) << 32) | surface;
        auto it = slots_.find(key);
        if (it == slots_.end()) return;
        it->second.active = false; ++it->second.revision; EnqueueLocked(key);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, slot] : slots_) {
            slot.active = false;
            ++slot.revision;
            EnqueueLocked(id);
        }
    }

    bool BindOutput(uint32_t id, uint64_t surfaceId)
    {
        if (!id || !surfaceId) return false;
        uint32_t pid = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = FindToplevelLocked(id);
            if (it == slots_.end() || !it->second.active) return false;
            pid = it->second.pid;
        }
        // Check the NCP registry without holding the controller lock: the
        // Wayland/child-ready paths obtain these locks in the opposite order.
        if (!WineIpcChildUsesDirectVulkan(static_cast<int32_t>(pid))) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = FindToplevelLocked(id);
        if (it == slots_.end() || !it->second.active || it->second.pid != pid)
            return false;
        Slot& slot = it->second;
        slot.outputSurfaceId = surfaceId;
        slot.outputWidth = 0;
        slot.outputHeight = 0;
        ++slot.outputRevision;
        boundOutputs_.insert(id);
        EnqueueLocked(it->first);
        OH_LOG_INFO(LOG_APP, "bind output top=%{public}u surface=%{public}llu",
                    id, static_cast<unsigned long long>(surfaceId));
        return true;
    }

    bool ResizeOutput(uint32_t id, int32_t width, int32_t height)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!boundOutputs_.count(id)) return false;
        auto it = FindToplevelLocked(id);
        if (it != slots_.end() && it->second.active && width > 0 && height > 0 &&
            (it->second.outputWidth != width || it->second.outputHeight != height)) {
            it->second.outputWidth = width;
            it->second.outputHeight = height;
            ++it->second.outputRevision;
            EnqueueLocked(it->first);
        }
        return true;
    }

    bool UnbindOutput(uint32_t id)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!boundOutputs_.erase(id)) return false;
        auto it = FindToplevelLocked(id);
        if (it == slots_.end() || !it->second.active) return true;
        Slot& slot = it->second;
        slot.outputSurfaceId = 0;
        ++slot.outputRevision;
        const uint64_t revision = slot.outputRevision;
        EnqueueLocked(it->first);
        if (slot.consumer) {
            // ArkUI may retire the XComponent immediately after this callback.
            // Drain the old Vulkan swapchain on the worker before returning.
            if (!condition_.wait_for(lock, std::chrono::seconds(5), [&] {
                    auto current = FindToplevelLocked(id);
                    return current == slots_.end() ||
                           current->second.outputAckRevision >= revision;
                }))
                OH_LOG_ERROR(LOG_APP, "unbind output timed out top=%{public}u", id);
        }
        return true;
    }

    bool SetDesktopConsumer(const void* owner, bool enabled, uint32_t scope)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled) {
            if (!desktopOwners_.Register(owner, scope)) return false;
        } else {
            desktopOwners_.Remove(owner);
            for (auto& [id, slot] : slots_)
                if (slot.readyOwner == owner) slot.readyOwner = nullptr;
        }
        condition_.notify_one();
        return true;
    }

    std::vector<winehua::direct::DirectDesktopSource> DesktopSources(const void* owner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<winehua::direct::DirectDesktopSource> out;
        if (!owner) return out;
        for (const auto& [id, slot] : slots_)
            if (slot.active && desktopOwners_.OwnerFor(slot.toplevelId) == owner &&
                slot.readyOwner == owner && slot.consumer &&
                slot.attached.generation && !slot.outputSurfaceId)
                out.push_back({slot.attached, slot.consumer});
        return out;
    }

private:
    auto FindToplevelLocked(uint32_t top) -> std::unordered_map<uint64_t, Slot>::iterator
    {
        return std::find_if(slots_.begin(), slots_.end(), [top](const auto& item) {
            return item.second.active && !item.second.desktopDrawable && item.second.toplevelId == top;
        });
    }

    void EnqueueLocked(uint64_t id)
    {
        if (queued_.insert(id).second) queue_.push_back(id);
        condition_.notify_one();
    }

    void Run()
    {
        for (;;) {
            uint64_t id = 0;
            std::vector<uint64_t> consumers;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(8),
                                    [this] { return !queue_.empty(); });
                if (!queue_.empty()) {
                    id = queue_.front();
                    queue_.pop_front();
                    queued_.erase(id);
                } else {
                    for (const auto& [top, slot] : slots_)
                        if (slot.active && slot.consumer && slot.attached.generation)
                            consumers.push_back(top);
                }
            }
            if (id) Process(id);
            else for (uint64_t top : consumers) Consume(top);
        }
    }

    void Consume(uint64_t id)
    {
        // The worker alone replaces/destroys consumer images. Lifecycle
        // callbacks only enqueue work, so this pointer stays valid here.
        std::shared_ptr<DirectImageQueue> image;
        uint64_t outputSurfaceId = 0;
        uint64_t outputRevision = 0;
        const void* desktop = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(id);
            if (it == slots_.end() || !it->second.active) return;
            image = it->second.consumer;
            outputSurfaceId = it->second.outputSurfaceId;
            outputRevision = it->second.outputRevision;
            desktop = !outputSurfaceId ? desktopOwners_.OwnerFor(it->second.toplevelId) : nullptr;
        }
        auto& state = consumers_[id];
        { std::lock_guard<std::mutex> lock(mutex_);
          auto it = slots_.find(id); if (it != slots_.end()) state.top = it->second.toplevelId; }
        if (state.outputRevision != outputRevision) {
            // Size changes keep the device/import cache and rebuild output on
            // the next frame. Rebinding/unbinding drains before acknowledging
            // the old surface, which ArkUI may then destroy immediately.
            if (state.presenter && outputSurfaceId && state.outputSurfaceId == outputSurfaceId)
                state.presenter->InvalidateOutput();
            else
                state.presenter.reset();
            state.outputSurfaceId = outputSurfaceId;
            state.outputRevision = outputRevision;
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(id);
            if (it != slots_.end())
                it->second.outputAckRevision = outputRevision;
            condition_.notify_all();
        }
        if (desktop) {
            state.presenter.reset(); // drain Vulkan reads before the desktop acquires
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(id);
            if (it != slots_.end() && it->second.consumer == image &&
                desktopOwners_.OwnerFor(it->second.toplevelId) == desktop)
                it->second.readyOwner = desktop;
            return;
        }
        if (!image) return;
        OHNativeWindowBuffer* windowBuffer = nullptr;
        int acquireFence = -1;
        if (!image->Acquire(&windowBuffer, &acquireFence)) {
            if (acquireFence >= 0) close(acquireFence);
            return;
        }
        OH_NativeWindow_NativeObjectReference(windowBuffer);
        if (!state.presenter)
            state.presenter = std::make_unique<winehua::direct::DirectVulkanPresenter>(state.outputSurfaceId);
        OH_NativeBuffer* nativeBuffer = nullptr;
        OH_NativeBuffer_Config config{};
        bool submitted = false;
        int releaseFence = -1;
        if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &nativeBuffer) == 0 &&
            nativeBuffer) OH_NativeBuffer_GetConfig(nativeBuffer, &config);
        if (nativeBuffer && config.width > 0 && config.height > 0) {
            // The presenter owns slot reuse, imported-image lifetime and output
            // rebuilds. BufferQueue receives its GPU release fence directly.
            submitted = state.presenter->Present(nativeBuffer, config.width, config.height,
                                                  &acquireFence, &releaseFence);
            if (!submitted) {
                OH_LOG_ERROR(LOG_APP,
                             "GPU consumer failed top=%{public}u frame=%{public}llu stage=%{public}s vk=%{public}d",
                             state.top, static_cast<unsigned long long>(state.presenter->FrameCount()),
                             state.presenter->Stage(), state.presenter->VkError());
                // A failed present may follow a successful GPU submit. Drain
                // before returning this buffer, including an export failure.
                state.presenter.reset();
            }
        }
        if (acquireFence >= 0) {
            // No submitted read consumed this acquire. Return the same fence
            // so recycling still waits for the producer, without CPU polling.
            if (releaseFence >= 0) close(releaseFence);
            releaseFence = acquireFence;
            acquireFence = -1;
        }
        const auto releaseStatus = image->ReleaseWithStatus(windowBuffer, releaseFence);
        if (releaseStatus == DirectImageQueue::ReleaseStatus::Error) {
            OH_LOG_ERROR(LOG_APP, "consumer release failed top=%{public}u", state.top);
        } else if (submitted && releaseStatus == DirectImageQueue::ReleaseStatus::Returned) {
            ++state.frames;
            if (state.frames == 1 || state.frames % 60 == 0)
                OH_LOG_INFO(LOG_APP,
                            "GPU consumer frames=%{public}llu top=%{public}u size=%{public}dx%{public}d presents=%{public}llu output=%{public}llu presenter=singleImage",
                            static_cast<unsigned long long>(state.frames), state.top,
                            config.width, config.height,
                            static_cast<unsigned long long>(state.presenter->OutputPresentCount()),
                            static_cast<unsigned long long>(state.outputSurfaceId));
        }
        OH_NativeWindow_NativeObjectUnreference(windowBuffer);
    }

    void Process(uint64_t id)
    {
        Slot snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(id);
            if (it == slots_.end()) return;
            snapshot = it->second;
        }
        if (!snapshot.active) {
            consumers_.erase(id);
            DirectSurfaceToken old{};
            std::shared_ptr<DirectImageQueue> oldImage;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = slots_.find(id);
                if (it == slots_.end() || it->second.active) return;
                old = it->second.attached;
                oldImage = it->second.consumer;
                slots_.erase(it);
            }
            if (old.generation) (void)DetachWineDirectSurface(old);
            oldImage.reset();
            condition_.notify_all();
            return;
        }
        if (!WineIpcChildUsesDirectVulkan(static_cast<int32_t>(snapshot.pid))) return;
        if (snapshot.attached.generation && snapshot.consumer &&
            snapshot.attached.clientPid == static_cast<int32_t>(snapshot.pid) &&
            snapshot.attached.wlSurfaceId == snapshot.wlSurfaceId &&
            snapshot.attached.generation == snapshot.generation) {
            if (snapshot.attached.width == snapshot.desiredWidth &&
                snapshot.attached.height == snapshot.desiredHeight) return;
            // Wine may already have a VkSurfaceKHR for this producer. Replacing
            // its ConsumerSurface on every xdg configure invalidates that
            // surface while a swapchain is presenting (SURFACE_LOST). Keep the
            // queue/generation and update only its size and NCP metadata.
            auto resized = snapshot.attached;
            resized.width = snapshot.desiredWidth;
            resized.height = snapshot.desiredHeight;
            if (snapshot.consumer->Resize(resized.width, resized.height) &&
                ResizeWineDirectSurface(resized)) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = slots_.find(id);
                if (it != slots_.end() && it->second.active &&
                    it->second.revision == snapshot.revision) {
                    it->second.attached = resized;
                    OH_LOG_INFO(LOG_APP,
                                "resized in place pid=%{public}u top=%{public}u gen=%{public}llu size=%{public}dx%{public}d",
                                snapshot.pid, snapshot.toplevelId,
                                static_cast<unsigned long long>(resized.generation),
                                resized.width, resized.height);
                }
            } else {
                // A failed metadata update must not destroy a live producer.
                OH_LOG_ERROR(LOG_APP,
                             "resize in place failed pid=%{public}u top=%{public}u gen=%{public}llu",
                             snapshot.pid, snapshot.toplevelId,
                             static_cast<unsigned long long>(resized.generation));
            }
            return;
        }

        OH_NativeImage* image = OH_ConsumerSurface_Create();
        OHNativeWindow* producer = nullptr;
        if (!image ||
            OH_ConsumerSurface_SetDefaultSize(image, snapshot.desiredWidth,
                                              snapshot.desiredHeight) != 0 ||
            OH_ConsumerSurface_SetDefaultUsage(image,
                NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE) != 0 ||
            !(producer = OH_NativeImage_AcquireNativeWindow(image))) {
            OH_LOG_ERROR(LOG_APP, "consumer create failed pid=%{public}u top=%{public}u",
                         snapshot.pid, id);
            if (image) OH_NativeImage_Destroy(&image);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(id);
            if (it == slots_.end() || !it->second.active ||
                it->second.revision != snapshot.revision) {
                OH_NativeImage_Destroy(&image);
                return;
            }
        }
        DirectSurfaceToken token{static_cast<int32_t>(snapshot.pid), snapshot.toplevelId,
                                 snapshot.wlSurfaceId, snapshot.generation,
                                 snapshot.desiredWidth, snapshot.desiredHeight};
        if (!AttachWineDirectSurface(token, producer)) {
            OH_LOG_WARN(LOG_APP,
                        "attach failed pid=%{public}u top=%{public}u wl=%{public}u gen=%{public}llu",
                        snapshot.pid, snapshot.toplevelId, snapshot.wlSurfaceId,
                        static_cast<unsigned long long>(token.generation));
            OH_NativeImage_Destroy(&image);
            return;
        }

        auto newImage = std::make_shared<DirectImageQueue>(image);
        std::shared_ptr<DirectImageQueue> oldImage;
        DirectSurfaceToken oldToken{};
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = slots_.find(id);
            if (it != slots_.end() && it->second.active &&
                it->second.revision == snapshot.revision) {
                oldImage = it->second.consumer;
                oldToken = it->second.attached;
                it->second.consumer = newImage;
                it->second.readyOwner = nullptr;
                it->second.attached = token;
                accepted = true;
            }
        }
        if (!accepted) {
            (void)DetachWineDirectSurface(token);
            return;
        }
        // Imported buffers from the previous generation must be idle and
        // unreferenced before its ConsumerSurface is destroyed.
        consumers_.erase(id);
        if (oldToken.generation && oldToken.clientPid != token.clientPid)
            (void)DetachWineDirectSurface(oldToken);
        oldImage.reset();
        OH_LOG_INFO(LOG_APP,
                    "attached pid=%{public}u top=%{public}u wl=%{public}u gen=%{public}llu size=%{public}dx%{public}d",
                    snapshot.pid, snapshot.toplevelId, snapshot.wlSurfaceId,
                    static_cast<unsigned long long>(token.generation),
                    token.width, token.height);
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<uint64_t, Slot> slots_;
    std::deque<uint64_t> queue_;
    std::unordered_set<uint64_t> queued_;
    std::unordered_map<uint64_t, ConsumerState> consumers_;
    std::unordered_set<uint32_t> boundOutputs_;
    std::atomic<uint64_t> nextGeneration_{1};
    winehua::direct::DirectConsumerScopes desktopOwners_;
};

Controller& GetController()
{
    static Controller* controller = new Controller();
    return *controller;
}

} // namespace

namespace winehua::direct {

DirectImageQueue::~DirectImageQueue()
{
    if (image_) OH_NativeImage_Destroy(&image_);
}

bool DirectImageQueue::Acquire(OHNativeWindowBuffer** buffer, int* fence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return OH_NativeImage_AcquireNativeWindowBuffer(image_, buffer, fence) == 0 && *buffer;
}

bool DirectImageQueue::Release(OHNativeWindowBuffer* buffer, int fence)
{
    return ReleaseWithStatus(buffer, fence) == ReleaseStatus::Returned;
}

DirectImageQueue::ReleaseStatus DirectImageQueue::ReleaseWithStatus(OHNativeWindowBuffer* buffer, int fence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int result = OH_NativeImage_ReleaseNativeWindowBuffer(image_, buffer, fence);
    if (result == 41210000) {
        // Producer disconnect/swapchain rebuild has removed this sequence.
        // Local GPU/image references must still be retired by the consumer.
        OH_LOG_INFO(LOG_APP, "queue buffer retired by producer result=%{public}d", result);
        return ReleaseStatus::RetiredByProducer;
    }
    if (result != 0)
        OH_LOG_WARN(LOG_APP, "queue buffer release result=%{public}d", result);
    return result == 0 ? ReleaseStatus::Returned : ReleaseStatus::Error;
}

bool DirectImageQueue::Resize(int width, int height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return OH_ConsumerSurface_SetDefaultSize(image_, width, height) == 0;
}

bool SetDirectDesktopConsumer(const void* owner, bool enabled, uint32_t scope)
{
    return GetController().SetDesktopConsumer(owner, enabled, scope);
}

std::vector<DirectDesktopSource> GetDirectDesktopSources(const void* owner)
{
    return GetController().DesktopSources(owner);
}

} // namespace winehua::direct

void DirectWineSurfaceCreated(uint32_t clientPid, uint32_t toplevelId,
                              uint32_t wlSurfaceId)
{
    GetController().Create(clientPid, toplevelId, wlSurfaceId);
}

uint64_t DirectWineDrawableDeclared(uint32_t pid, uint32_t top, uint32_t wl,
                                   int32_t width, int32_t height)
{
    return GetController().Create(pid, top, wl, width, height, true);
}

void DirectWineDrawableDestroyed(uint32_t pid, uint32_t wl)
{
    GetController().DestroyDrawable(pid, wl);
}

void DirectWineSurfaceResized(uint32_t toplevelId, int32_t width, int32_t height)
{
    GetController().Resize(toplevelId, width, height);
}

void DirectWineSurfaceDestroyed(uint32_t toplevelId)
{
    GetController().Destroy(toplevelId);
}

void DirectWineSurfaceChildReady(uint32_t clientPid)
{
    GetController().ChildReady(clientPid);
}

void DirectWineSurfaceReset()
{
    GetController().Reset();
}

bool DirectWineSurfaceBindOutput(uint32_t toplevelId, uint64_t surfaceId)
{
    return GetController().BindOutput(toplevelId, surfaceId);
}

bool DirectWineSurfaceResizeOutput(uint32_t toplevelId, int32_t width, int32_t height)
{
    return GetController().ResizeOutput(toplevelId, width, height);
}

bool DirectWineSurfaceUnbindOutput(uint32_t toplevelId)
{
    return GetController().UnbindOutput(toplevelId);
}
