#include "graphics/native_window_gles_target.h"
#include "graphics/present_pacing.h"
#include <native_buffer/native_buffer.h>
#include <native_window/graphic_error_code.h>
#include <unistd.h>

namespace winehua {
namespace {
template<class T> T Proc(const char* name) { return reinterpret_cast<T>(eglGetProcAddress(name)); }
}

bool NativeWindowGlesTarget::Fail(const char* reason, int32_t error)
{
    reason_ = reason;
    error_ = error;
    return false;
}

bool NativeWindowGlesTarget::Configure(const GlesBufferOwner& owner, OHNativeWindow* window)
{
    if (Ready()) return owner_ == owner && window_ == window;
    display_ = reinterpret_cast<EGLDisplay>(owner.display);
    if (!window || !owner.surfaceKey || !owner.generation || !owner.width || !owner.height ||
        eglGetCurrentDisplay() != display_ ||
        eglGetCurrentContext() != reinterpret_cast<EGLContext>(owner.context))
        return Fail("invalid-context-owner");
    const char* eglExtensions = eglQueryString(display_, EGL_EXTENSIONS);
    const char* glExtensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const std::string_view eglList = eglExtensions ? eglExtensions : "";
    const std::string_view glList = glExtensions ? glExtensions : "";
    for (const char* extension : {"EGL_OHOS_image_native_buffer", "EGL_KHR_image_base",
            "EGL_ANDROID_native_fence_sync", "EGL_KHR_wait_sync"})
        if (!HasGlExtension(eglList, extension)) return Fail(extension);
    if (!HasGlExtension(glList, "GL_OES_EGL_image")) return Fail("GL_OES_EGL_image");
    createImage_ = Proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    destroyImage_ = Proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    imageToRenderbuffer_ = Proc<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>("glEGLImageTargetRenderbufferStorageOES");
    createSync_ = Proc<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
    destroySync_ = Proc<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR");
    waitSync_ = Proc<PFNEGLWAITSYNCKHRPROC>("eglWaitSyncKHR");
    dupFence_ = Proc<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>("eglDupNativeFenceFDANDROID");
    if (!createImage_ || !destroyImage_ || !imageToRenderbuffer_ || !createSync_ ||
        !destroySync_ || !waitSync_ || !dupFence_) return Fail("missing-entrypoint");
    if (OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY,
            static_cast<int32_t>(owner.width), static_cast<int32_t>(owner.height)) ||
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, owner.format) ||
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE,
            static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE)))
        return Fail("window-configuration");
    owner_ = owner;
    window_ = window;
    timeoutMs_ = -1;
    reason_ = "ready";
    return true;
}

NativeWindowGlesTarget::Slot* NativeWindowGlesTarget::Import(OHNativeWindowBuffer* buffer)
{
    OH_NativeBuffer* native = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native) || !native) {
        Fail("native-buffer");
        return nullptr;
    }
    OH_NativeBuffer_Config config{};
    OH_NativeBuffer_GetConfig(native, &config);
    if (config.width != static_cast<int32_t>(owner_.width) ||
        config.height != static_cast<int32_t>(owner_.height) || config.format != owner_.format) {
        Fail("buffer-layout-mismatch");
        return nullptr;
    }
    const uint32_t seq = OH_NativeBuffer_GetSeqNum(native);
    for (size_t i = 0; i < slotCount_; ++i) {
        Slot& slot = slots_[i];
        if (slot.seq == seq && slot.owner == owner_) return &slot;
    }
    if (slotCount_ == slots_.size()) { Fail("import-limit"); return nullptr; }
    Slot slot;
    slot.owner = owner_;
    slot.seq = seq;
    if (OH_NativeWindow_NativeObjectReference(buffer)) { Fail("buffer-reference"); return nullptr; }
    slot.buffer = buffer;
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    slot.image = createImage_(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                              reinterpret_cast<EGLClientBuffer>(buffer), attributes);
    if (slot.image == EGL_NO_IMAGE_KHR) {
        Fail("egl-image-import", eglGetError());
        DestroySlot(slot);
        return nullptr;
    }
    glGenRenderbuffers(1, &slot.renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, slot.renderbuffer);
    imageToRenderbuffer_(GL_RENDERBUFFER, slot.image);
    glGenFramebuffers(1, &slot.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, slot.renderbuffer);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const GLenum error = glGetError();
    if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
        Fail("incomplete-import-framebuffer", error != GL_NO_ERROR ? error : status);
        DestroySlot(slot);
        return nullptr;
    }
    slots_[slotCount_] = slot;
    return &slots_[slotCount_++];
}

bool NativeWindowGlesTarget::WaitAcquire(int fd)
{
    if (fd < 0) return true;
    const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSyncKHR sync = createSync_(display_, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync == EGL_NO_SYNC_KHR) {
        close(fd); // EGL owns the fd only after successful import.
        return Fail("acquire-fence-import", eglGetError());
    }
    const EGLint waited = waitSync_(display_, sync, 0);
    destroySync_(display_, sync);
    return waited == EGL_TRUE || Fail("acquire-gpu-wait", eglGetError());
}

GlesBeginResult NativeWindowGlesTarget::BeginFrame(uint64_t frames, uint64_t timestampNs)
{
    if (!Ready() || pendingBuffer_) { Fail("outstanding-frame"); return GlesBeginResult::Failed; }
    const int32_t timeout = static_cast<int32_t>(DirectPresentAcquireTimeoutNs(frames) / 1000000);
    if (timeoutMs_ != timeout) {
        error_ = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, timeout);
        if (error_) { Fail("request-timeout-configuration", error_); return GlesBeginResult::Failed; }
        timeoutMs_ = timeout;
    }
    // Stamp before RequestBuffer: SurfaceQueue snapshots the UI timestamp when
    // it dequeues the wrapper, not when FlushBuffer is called.
    error_ = OH_NativeWindow_NativeWindowHandleOpt(window_, SET_UI_TIMESTAMP, timestampNs);
    if (error_) { Fail("frame-timestamp", error_); return GlesBeginResult::Failed; }
    int fd = -1;
    error_ = OH_NativeWindow_NativeWindowRequestBuffer(window_, &pendingBuffer_, &fd);
    if (error_ || !pendingBuffer_) {
        if (fd >= 0) close(fd);
        pendingBuffer_ = nullptr;
        if (error_ == NATIVE_ERROR_NO_BUFFER || error_ == NATIVE_ERROR_BUFFER_QUEUE_FULL) {
            reason_ = "queue-full";
            return GlesBeginResult::Deferred;
        }
        Fail("request-buffer", error_);
        return GlesBeginResult::Failed;
    }
    current_ = Import(pendingBuffer_);
    if (!current_) {
        if (fd >= 0) close(fd);
        AbortFrame();
        return GlesBeginResult::Failed;
    }
    if (!WaitAcquire(fd)) { AbortFrame(); return GlesBeginResult::Failed; }
    // The acquire GPU wait orders old writes before the new draw. A new
    // completion fence will cover both; deletion of the old GLsync is safe.
    if (current_->completion) glDeleteSync(current_->completion);
    current_->completion = nullptr;
    glBindFramebuffer(GL_FRAMEBUFFER, current_->framebuffer);
    writesPending_ = true;
    return GlesBeginResult::Ready;
}

bool NativeWindowGlesTarget::Publish()
{
    if (!current_ || !pendingBuffer_ || !writesPending_) return Fail("no-frame-to-publish");
    current_->completion = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    EGLSyncKHR release = createSync_(display_, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    glFlush(); // Submit both fences before exporting; no CPU wait/glFinish.
    if (!current_->completion || release == EGL_NO_SYNC_KHR) {
        if (release != EGL_NO_SYNC_KHR) destroySync_(display_, release);
        return Fail("render-fence-create", eglGetError());
    }
    const int fd = dupFence_(display_, release);
    destroySync_(display_, release);
    if (fd < 0) return Fail("render-fence-export", eglGetError());
    Region damage{};
    // For valid window/buffer arguments SurfaceQueue consumes the fd even if
    // FlushBuffer fails. Closing it again can close an unrelated reused fd.
    error_ = OH_NativeWindow_NativeWindowFlushBuffer(window_, pendingBuffer_, fd, damage);
    if (error_) return Fail("flush-buffer", error_);
    current_ = nullptr;
    pendingBuffer_ = nullptr;
    writesPending_ = false;
    return true;
}

bool NativeWindowGlesTarget::RetireWrites(Slot& slot)
{
    if (!slot.completion) return true;
    const GLenum status = glClientWaitSync(slot.completion, 0, 0);
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) return false;
    glDeleteSync(slot.completion);
    slot.completion = nullptr;
    return true;
}

bool NativeWindowGlesTarget::AbortFrame()
{
    if (!pendingBuffer_) return true;
    if (writesPending_ && current_) {
        if (!current_->completion) {
            current_->completion = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            if (!current_->completion) return false;
        }
        if (!RetireWrites(*current_)) return false;
    }
    if (OH_NativeWindow_NativeWindowAbortBuffer(window_, pendingBuffer_)) return false;
    pendingBuffer_ = nullptr;
    current_ = nullptr;
    writesPending_ = false;
    return true;
}

void NativeWindowGlesTarget::DestroySlot(Slot& slot)
{
    if (slot.framebuffer) glDeleteFramebuffers(1, &slot.framebuffer);
    if (slot.renderbuffer) glDeleteRenderbuffers(1, &slot.renderbuffer);
    if (slot.image != EGL_NO_IMAGE_KHR) destroyImage_(display_, slot.image);
    if (slot.buffer) OH_NativeWindow_NativeObjectUnreference(slot.buffer);
    slot = {};
}

bool NativeWindowGlesTarget::Reset()
{
    if (Ready() && (eglGetCurrentDisplay() != display_ ||
        eglGetCurrentContext() != reinterpret_cast<EGLContext>(owner_.context))) return false;
    if (!AbortFrame()) return false;
    for (size_t i = 0; i < slotCount_; ++i) if (!RetireWrites(slots_[i])) return false;
    for (size_t i = 0; i < slotCount_; ++i) DestroySlot(slots_[i]);
    slotCount_ = 0;
    window_ = nullptr;
    display_ = EGL_NO_DISPLAY;
    owner_ = {};
    timeoutMs_ = -1;
    return true;
}
} // namespace winehua
