#pragma once
#include "graphics/gles_direct_policy.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <native_window/external_window.h>
#include <array>

namespace winehua {
enum class GlesBeginResult { Ready, Deferred, Failed };

// Imports belong to exactly one presenter context/window generation. This class
// does not own the EGL context or OHNativeWindow: the presenter keeps them alive
// until Reset succeeds. No cache eviction while a consumer can retain a buffer.
class NativeWindowGlesTarget {
public:
    NativeWindowGlesTarget() = default;
    NativeWindowGlesTarget(const NativeWindowGlesTarget&) = delete;
    NativeWindowGlesTarget& operator=(const NativeWindowGlesTarget&) = delete;
    bool Configure(const GlesBufferOwner& owner, OHNativeWindow* window);
    GlesBeginResult BeginFrame(uint64_t presentedFrames, uint64_t timestampNs);
    bool Publish();
    // Never cancel/recycle an image while GPU writes to it. Reset is nonblocking;
    // false means keep context/window/imports alive and retry at the next frame.
    bool AbortFrame();
    bool Reset();
    bool Ready() const { return window_ != nullptr; }
    const char* Reason() const { return reason_; }
    int32_t Error() const { return error_; }
    size_t ImportedSlots() const { return slotCount_; }

private:
    struct Slot {
        GlesBufferOwner owner;
        uint32_t seq = 0;
        OHNativeWindowBuffer* buffer = nullptr; // explicitly referenced
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint renderbuffer = 0, framebuffer = 0;
        GLsync completion = nullptr;
    };
    Slot* Import(OHNativeWindowBuffer* buffer);
    bool WaitAcquire(int fd);
    bool RetireWrites(Slot& slot);
    void DestroySlot(Slot& slot);
    bool Fail(const char* reason, int32_t error = 0);

    GlesBufferOwner owner_;
    OHNativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    std::array<Slot, 64> slots_{};
    size_t slotCount_ = 0;
    Slot* current_ = nullptr;
    OHNativeWindowBuffer* pendingBuffer_ = nullptr;
    bool writesPending_ = false;
    int32_t timeoutMs_ = -1;
    const char* reason_ = "unconfigured";
    int32_t error_ = 0;
    PFNEGLCREATEIMAGEKHRPROC createImage_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage_ = nullptr;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageToRenderbuffer_ = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync_ = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync_ = nullptr;
    PFNEGLWAITSYNCKHRPROC waitSync_ = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence_ = nullptr;
};
} // namespace winehua
