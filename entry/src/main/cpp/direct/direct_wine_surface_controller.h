#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <native_image/native_image.h>
#include "proc/wine_child_ipc.h"

namespace winehua::direct {

// Shared queue ownership keeps the consumer alive until its last GPU frame
// has been returned, even when Wayland destroys the corresponding window.
class DirectImageQueue {
public:
    explicit DirectImageQueue(OH_NativeImage* image) : image_(image) {}
    ~DirectImageQueue();
    DirectImageQueue(const DirectImageQueue&) = delete;
    bool Acquire(OHNativeWindowBuffer** buffer, int* fence);
    // Release always transfers fence ownership, including error returns.
    bool Release(OHNativeWindowBuffer* buffer, int fence);
    enum class ReleaseStatus { Returned, RetiredByProducer, Error };
    ReleaseStatus ReleaseWithStatus(OHNativeWindowBuffer* buffer, int fence);
    bool Resize(int width, int height);
private:
    std::mutex mutex_;
    OH_NativeImage* image_;
};

struct DirectDesktopSource {
    wineipc::DirectSurfaceToken token;
    std::shared_ptr<DirectImageQueue> queue;
};

// One desktop renderer owns queue consumption; the production Vulkan presenter
// worker drains its old submissions before publishing desktop sources.
void SetDirectDesktopConsumer(const void* owner, bool enabled);
std::vector<DirectDesktopSource> GetDirectDesktopSources(const void* owner);

} // namespace winehua::direct

// App-side control plane for the opt-in Wine Direct Vulkan NCP. The consumer
// queue belongs to the App; only its producer NativeWindow crosses IPC.
void DirectWineSurfaceCreated(uint32_t clientPid, uint32_t toplevelId,
                              uint32_t wlSurfaceId);
void DirectWineSurfaceResized(uint32_t toplevelId, int32_t width, int32_t height);
void DirectWineSurfaceDestroyed(uint32_t toplevelId);
void DirectWineSurfaceChildReady(uint32_t clientPid);
void DirectWineSurfaceReset();

// A managed Wine window has its own XComponent. Return true only when this
// toplevel belongs to an opted-in Direct NCP, so EGL never owns the same
// output surface as the Vulkan consumer.
bool DirectWineSurfaceBindOutput(uint32_t toplevelId, uint64_t surfaceId);
bool DirectWineSurfaceResizeOutput(uint32_t toplevelId, int32_t width, int32_t height);
bool DirectWineSurfaceUnbindOutput(uint32_t toplevelId);
