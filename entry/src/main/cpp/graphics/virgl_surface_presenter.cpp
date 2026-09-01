#include "virgl_surface_presenter.h"
#include "venus_surface_presenter.h"
#include "native_window_lease.h"
#include "present_target.h"
#include "presenter_common.h"
#include "shader_utils.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "virgl-presenter"

namespace {

// 帧周期常量与工具函数收编于 presenter_common.h (行为平价 — 逻辑与返回值不变)
using winehua::SteadyClock;
using winehua::kDefaultFramePeriodNs;
using winehua::kMinFramePeriodNs;
using winehua::kVirglMaxFramePeriodNs;
using winehua::kDispatchLeadNs;
using winehua::NormalizeVirglFramePeriodNs;
using winehua::VirglPacingPeriodNs;
using winehua::NowUs;
using winehua::NowNs;
using winehua::PresentPerfSummaryEnabled;
using winehua::PresentTarget;
// 返回码命名化 (数值与旧实现逐点一致, 消费者 virgl_child.cpp 的
// < -2 且 != -6 日志门控语义保留)
using winehua::kPresentOk;
using winehua::kPresentThrottled;
using winehua::kPresentNoTarget;
using winehua::kPresentSourceInvisible;
using winehua::kPresentGlSetupFailed;
using winehua::kPresentMakeCurrentFailed;
using winehua::kPresentBlitFailed;
using winehua::kPresentFenceSyncFailed;
using winehua::kPresentInvalid;

constexpr auto kVenusTargetAttachTimeout = std::chrono::milliseconds(2500);

GLuint CompilePresentShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    OH_LOG_ERROR(LOG_APP, "[VIRGL-ZC][NCP] shader compile failed: %{public}s", log);
    glDeleteShader(shader);
    return 0;
}

// virgl 呈现目标 (GL blit)。实现 PresentTarget 接口 (见 present_target.h):
// 支持 Present, 其 PresentVenus 为防御性死路径返回 kPresentInvalid。
class SurfaceQueueTarget : public winehua::PresentTarget {
public:
    bool IsVulkan() const override { return false; }

    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs,
               OHNativeWindow* window, bool releaseWindowWithUnreference) override
    {
        if (!surfaceKey || !window) return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        ResetGlLocked();
        windowLease_.Adopt(
            window, releaseWindowWithUnreference
                ? winehua::NativeWindowReleaseMode::UnreferenceNativeObject
                : winehua::NativeWindowReleaseMode::DestroyParcelWindow);
        surfaceKey_ = surfaceKey;
        width_ = 0;
        height_ = 0;
        frames_ = 0;
        failures_ = 0;
        timestampFailures_ = 0;
        throttled_ = 0;
        lastPresentNs_ = 0;
        displayPeriodNs_ = NormalizeVirglFramePeriodNs(framePeriodNs);
        framePeriodNs_ = VirglPacingPeriodNs(displayPeriodNs_);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] target attached surface_key=%{public}llu "
                    "window=%{public}p display_period_us=%{public}llu "
                    "pace_period_us=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_), windowLease_.Get(),
                    static_cast<unsigned long long>(displayPeriodNs_ / 1000),
                    static_cast<unsigned long long>(framePeriodNs_ / 1000));
        return 0;
    }

    int SetFramePeriod(uint64_t framePeriodNs) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t displayPeriodNs = NormalizeVirglFramePeriodNs(framePeriodNs);
        if (displayPeriodNs_ == displayPeriodNs) return 0;
        displayPeriodNs_ = displayPeriodNs;
        framePeriodNs_ = VirglPacingPeriodNs(displayPeriodNs_);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] frame period surface_key=%{public}llu "
                    "display_period_us=%{public}llu pace_period_us=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_),
                    static_cast<unsigned long long>(displayPeriodNs_ / 1000),
                    static_cast<unsigned long long>(framePeriodNs_ / 1000));
        return 0;
    }

    int Detach(uint64_t surfaceKey) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (surfaceKey_ && surfaceKey && surfaceKey_ != surfaceKey) return -1;
        ResetLocked();
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][NCP] target detached surface_key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        return 0;
    }

    int Present(GLuint texture, uint32_t width, uint32_t height,
                uint64_t drawable, uint32_t serial,
                uint64_t* nextPresentDeadlineNs) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nextPresentDeadlineNs) *nextPresentDeadlineNs = 0;
        const EGLDisplay sourceDisplay = eglGetCurrentDisplay();
        const EGLContext sourceContext = eglGetCurrentContext();
        const EGLSurface sourceDraw = eglGetCurrentSurface(EGL_DRAW);
        const EGLSurface sourceRead = eglGetCurrentSurface(EGL_READ);
        const bool sourceVisible = sourceDisplay != EGL_NO_DISPLAY &&
            sourceContext != EGL_NO_CONTEXT && texture != 0 &&
            glIsTexture(texture) == GL_TRUE;
        GLsync sourceReady = nullptr;

        if (!windowLease_) return kPresentNoTarget;
        if (!sourceVisible) return kPresentSourceInvisible;
        const uint64_t nowNs = NowNs();
        if (width_ == width && height_ == height && lastPresentNs_ &&
            nowNs - lastPresentNs_ < framePeriodNs_)
        {
            if (nextPresentDeadlineNs)
                *nextPresentDeadlineNs = lastPresentNs_ + framePeriodNs_;
            ++throttled_;
            return kPresentThrottled;
        }
        lastPresentNs_ = nowNs;
        sourceReady = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sourceReady) return kPresentFenceSyncFailed;
        glFlush();
        if (!EnsureGlLocked(sourceDisplay, sourceContext, width, height))
        {
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            glDeleteSync(sourceReady);
            ++failures_;
            return kPresentGlSetupFailed;
        }
        if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE)
        {
            eglMakeCurrent(sourceDisplay, sourceDraw, sourceRead, sourceContext);
            glDeleteSync(sourceReady);
            ++failures_;
            return kPresentMakeCurrentFailed;
        }
        glWaitSync(sourceReady, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(sourceReady);

        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, sampler_);
        glUniform1i(textureLocation_, 0);
        const uint64_t frameTimestamp = NowNs();
        const int32_t timestampResult = OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_UI_TIMESTAMP, frameTimestamp);
        if (timestampResult != 0)
        {
            ++timestampFailures_;
            if (timestampFailures_ == 1 || timestampFailures_ % 120 == 0)
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][NCP] timestamp set failed serial=%{public}u "
                            "result=%{public}d failures=%{public}llu",
                            serial, timestampResult,
                            static_cast<unsigned long long>(timestampFailures_));
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const GLenum glError = glGetError();
        const EGLBoolean swapped = glError == GL_NO_ERROR
            ? eglSwapBuffers(display_, surface_) : EGL_FALSE;
        const EGLint eglError = swapped == EGL_TRUE ? EGL_SUCCESS : eglGetError();
        const EGLBoolean restored = eglMakeCurrent(
            sourceDisplay, sourceDraw, sourceRead, sourceContext);

        if (swapped != EGL_TRUE || restored != EGL_TRUE)
        {
            ++failures_;
            if (failures_ == 1 || failures_ % 120 == 0)
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][NCP] blit dropped serial=%{public}u gl=0x%{public}x "
                            "egl=0x%{public}x restore=%{public}d drops=%{public}llu",
                            serial, glError, eglError, restored,
                            static_cast<unsigned long long>(failures_));
            return kPresentBlitFailed;
        }

        ++frames_;
        if (nextPresentDeadlineNs)
            *nextPresentDeadlineNs = lastPresentNs_ + framePeriodNs_;
        if (PresentPerfSummaryEnabled() &&
            (frames_ == 1 || frames_ % 120 == 0))
        {
            OH_LOG_INFO(LOG_APP,
                        "[VIRGL-ZC][NCP] blit frames=%{public}llu surface_key=%{public}llu "
                        "serial=%{public}u drawable=0x%{public}llx tex=%{public}u "
                        "size=%{public}ux%{public}u display_period_us=%{public}llu "
                        "pace_period_us=%{public}llu "
                        "drops=%{public}llu throttled=%{public}llu",
                        static_cast<unsigned long long>(frames_),
                        static_cast<unsigned long long>(surfaceKey_), serial,
                        static_cast<unsigned long long>(drawable), texture, width, height,
                        static_cast<unsigned long long>(displayPeriodNs_ / 1000),
                        static_cast<unsigned long long>(framePeriodNs_ / 1000),
                        static_cast<unsigned long long>(failures_),
                        static_cast<unsigned long long>(throttled_));
        }
        return kPresentOk;
    }

    // PresentTarget 接口 (见 present_target.h): venus-only 方法与 GL 死路径。
    int PresentVenus(uint32_t /*contextId*/, uintptr_t /*instance*/,
                     uintptr_t /*physicalDevice*/, uintptr_t /*device*/,
                     uintptr_t /*queue*/, uint64_t /*image*/,
                     uint32_t /*queueFamily*/, uint32_t /*width*/,
                     uint32_t /*height*/, uint32_t /*format*/, uint32_t /*layout*/,
                     uint32_t /*serial*/,
                     uint64_t* /*nextPresentDeadlineNs*/,
                     void (*)(void*), void*) override
    {
        return kPresentInvalid;
    }
    bool HasVulkanDevice() override { return false; }
    bool PrepareDeviceRelease(uint32_t, uintptr_t) override { return false; }
    bool FinishDeviceRelease(uint32_t, uintptr_t, int32_t) override { return false; }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetLocked();
    }

private:
    bool EnsureGlLocked(EGLDisplay sourceDisplay, EGLContext sourceContext,
                        uint32_t width, uint32_t height)
    {
        if (display_ == sourceDisplay && sourceContext_ == sourceContext &&
            context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE &&
            (width_ != width || height_ != height)) {
            // Resizing a window surface only changes future buffer requests.
            // Recreating EGL here also tears down the producer queue while the
            // NativeImage consumer may still own an acquired buffer.
            if (OH_NativeWindow_NativeWindowHandleOpt(
                    windowLease_.Get(), SET_BUFFER_GEOMETRY,
                    static_cast<int32_t>(width), static_cast<int32_t>(height)) != 0)
                return false;
            width_ = width;
            height_ = height;
            OH_LOG_INFO(LOG_APP,
                        "[VIRGL-ZC][NCP] window resized size=%{public}ux%{public}u "
                        "surface_key=%{public}llu retained_egl=1",
                        width, height, static_cast<unsigned long long>(surfaceKey_));
            return true;
        }
        if (display_ != EGL_NO_DISPLAY &&
            (display_ != sourceDisplay || sourceContext_ != sourceContext ||
             width_ != width || height_ != height))
            ResetGlLocked();
        if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE) return true;

        if (OH_NativeWindow_NativeWindowHandleOpt(
                windowLease_.Get(), SET_BUFFER_GEOMETRY,
                static_cast<int32_t>(width), static_cast<int32_t>(height)) != 0)
            return false;
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_USAGE,
            static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE));
        const int32_t timeoutResult = OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), SET_TIMEOUT, static_cast<int32_t>(0));
        int32_t queueSize = 0;
        OH_NativeWindow_NativeWindowHandleOpt(
            windowLease_.Get(), GET_BUFFERQUEUE_SIZE, &queueSize);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][NCP] window configured size=%{public}ux%{public}u "
                    "queue=%{public}d timeout_ms=0 timeout_ret=%{public}d",
                    width, height, queueSize, timeoutResult);

        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE,
        };
        EGLint configCount = 0;
        EGLConfig config = nullptr;
        if (!eglChooseConfig(sourceDisplay, configAttributes, &config, 1, &configCount) ||
            configCount == 0)
            return false;

        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext context = eglCreateContext(
            sourceDisplay, config, sourceContext, contextAttributes);
        if (context == EGL_NO_CONTEXT) return false;
        EGLSurface surface = eglCreateWindowSurface(
            sourceDisplay, config,
            reinterpret_cast<EGLNativeWindowType>(windowLease_.Get()), nullptr);
        if (surface == EGL_NO_SURFACE)
        {
            eglDestroyContext(sourceDisplay, context);
            return false;
        }
        if (eglMakeCurrent(sourceDisplay, surface, surface, context) != EGL_TRUE)
        {
            eglDestroySurface(sourceDisplay, surface);
            eglDestroyContext(sourceDisplay, context);
            return false;
        }

        // 全屏 quad GLSL 收编于 shader_utils (重构第 3 步, 行为平价 — 逐字搬移,
        // 仅由内嵌字符串改为 shader_utils 命名常量统一存放)
        const GLuint vertex = CompilePresentShader(
            GL_VERTEX_SHADER, winehua::kPresentFullscreenQuadVS);
        const GLuint fragment = CompilePresentShader(
            GL_FRAGMENT_SHADER, winehua::kPresentFullscreenQuadFS);
        GLuint program = 0;
        if (vertex && fragment)
        {
            program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE)
            {
                char log[512] = {};
                glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                OH_LOG_ERROR(LOG_APP, "[VIRGL-ZC][NCP] program link failed: %{public}s", log);
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        if (!program)
        {
            eglMakeCurrent(sourceDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(sourceDisplay, surface);
            eglDestroyContext(sourceDisplay, context);
            return false;
        }

        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        eglSwapInterval(sourceDisplay, 0);

        display_ = sourceDisplay;
        sourceContext_ = sourceContext;
        context_ = context;
        surface_ = surface;
        program_ = program;
        sampler_ = sampler;
        textureLocation_ = glGetUniformLocation(program_, "uTexture");
        width_ = width;
        height_ = height;
        return true;
    }

    void ResetGlLocked()
    {
        if (display_ != EGL_NO_DISPLAY)
        {
            const EGLDisplay previousDisplay = eglGetCurrentDisplay();
            const EGLContext previousContext = eglGetCurrentContext();
            const EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
            const EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
            const bool cleanupCurrent = context_ != EGL_NO_CONTEXT &&
                surface_ != EGL_NO_SURFACE &&
                eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
            if (cleanupCurrent)
            {
                if (sampler_) glDeleteSamplers(1, &sampler_);
                if (program_) glDeleteProgram(program_);
                if (previousDisplay != EGL_NO_DISPLAY)
                    eglMakeCurrent(previousDisplay, previousDraw, previousRead, previousContext);
                else
                    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        }
        display_ = EGL_NO_DISPLAY;
        sourceContext_ = EGL_NO_CONTEXT;
        context_ = EGL_NO_CONTEXT;
        surface_ = EGL_NO_SURFACE;
        program_ = 0;
        sampler_ = 0;
        textureLocation_ = -1;
        width_ = 0;
        height_ = 0;
    }

    void ResetLocked()
    {
        ResetGlLocked();
        ReleaseWindowLocked();
        surfaceKey_ = 0;
    }

    void ReleaseWindowLocked()
    {
        windowLease_.Reset();
    }

    std::mutex mutex_;
    winehua::NativeWindowLease windowLease_;
    uint64_t surfaceKey_ = 0;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext sourceContext_ = EGL_NO_CONTEXT;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    GLuint program_ = 0;
    GLuint sampler_ = 0;
    GLint textureLocation_ = -1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t frames_ = 0;
    uint64_t failures_ = 0;
    uint64_t timestampFailures_ = 0;
    uint64_t throttled_ = 0;
    uint64_t lastPresentNs_ = 0;
    uint64_t displayPeriodNs_ = kDefaultFramePeriodNs;
    uint64_t framePeriodNs_ = kDefaultFramePeriodNs;
};

class SurfaceQueuePresenterManager {
public:
    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs, uint32_t flags,
               OHNativeWindow* window)
    {
        if (!surfaceKey || !window) return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = surfaces_[surfaceKey];
        entry.missingTargetLogged = false;
        const bool releaseWindowWithUnreference =
            (flags & winehua::virgl_ipc::kSurfaceNativeObjectReference) != 0;
        entry.info.flags =
            (entry.info.flags & ~(winehua::virgl_ipc::kSurfaceVulkan |
                                  winehua::virgl_ipc::kSurfaceAttached)) |
            (flags & winehua::virgl_ipc::kSurfaceVulkan);
        int result;
        const bool vulkan =
            (entry.info.flags & winehua::virgl_ipc::kSurfaceVulkan) != 0;
        if (vulkan)
        {
            // 旧 virgl (GL) target: detach + 立即释放 (无延迟释放机制)。
            if (entry.target && !entry.target->IsVulkan()) {
                entry.target->Detach(surfaceKey);
                entry.target.reset();
            }
            // 旧 venus target: detach + (有 device → 延迟释放 / 否则立即)。
            RetireTargetLocked(surfaceKey, entry.target);
            entry.target = std::make_unique<winehua::VenusSurfaceQueueTarget>();
            result = entry.target->Attach(surfaceKey, framePeriodNs, window,
                                          releaseWindowWithUnreference);
        }
        else
        {
            // 旧 venus target: detach + 延迟/立即释放; virgl target 复用。
            RetireTargetLocked(surfaceKey, entry.target);
            if (!entry.target)
                entry.target = std::make_unique<SurfaceQueueTarget>();
            result = entry.target->Attach(surfaceKey, framePeriodNs, window,
                                          releaseWindowWithUnreference);
        }
        if (result == 0) {
            entry.info.flags |= winehua::virgl_ipc::kSurfaceAttached;
            targetCondition_.notify_all();
        }
        return result;
    }

    int Detach(uint64_t surfaceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(surfaceKey);
        if (it == surfaces_.end()) return 0;
        if (it->second.target) {
            it->second.target->Detach(surfaceKey);
            // venus 持 device 时延迟释放 (待 device-owner callback); 否则随
            // entry 析构立即释放 (virgl 无延迟释放机制)。
            if (it->second.target->HasVulkanDevice())
                retiredVenusTargets_.push_back(std::move(it->second.target));
        }
        ++surfaceGenerations_[surfaceKey];
        surfaces_.erase(it);
        targetCondition_.notify_all();
        return 0;
    }

    int PrepareVenusDeviceRelease(uint32_t contextId, uintptr_t device)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int matches = 0;
        for (auto& [surfaceKey, entry] : surfaces_)
        {
            static_cast<void>(surfaceKey);
            if (entry.target && entry.target->PrepareDeviceRelease(contextId, device))
                ++matches;
        }
        for (auto& target : retiredVenusTargets_)
        {
            if (target->PrepareDeviceRelease(contextId, device)) ++matches;
        }
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] device release prepare complete "
                    "ctx=%{public}u device=0x%{public}llx targets=%{public}d",
                    contextId, static_cast<unsigned long long>(device), matches);
        return matches;
    }

    int FinishVenusDeviceRelease(uint32_t contextId, uintptr_t device,
                                 int32_t waitResult)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int matches = 0;
        for (auto& [surfaceKey, entry] : surfaces_)
        {
            static_cast<void>(surfaceKey);
            if (entry.target && entry.target->FinishDeviceRelease(
                    contextId, device, waitResult))
                ++matches;
        }
        for (auto& target : retiredVenusTargets_)
        {
            if (target->FinishDeviceRelease(contextId, device, waitResult)) ++matches;
        }
        retiredVenusTargets_.erase(
            std::remove_if(retiredVenusTargets_.begin(), retiredVenusTargets_.end(),
                           [](const auto& target) {
                               return !target->HasVulkanDevice();
                           }),
            retiredVenusTargets_.end());
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] device release after-wait complete "
                    "ctx=%{public}u device=0x%{public}llx wait_result=%{public}d "
                    "targets=%{public}d",
                    contextId, static_cast<unsigned long long>(device),
                    waitResult, matches);
        return matches;
    }

    int SetFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(surfaceKey);
        if (it == surfaces_.end()) return kPresentNoTarget;
        // target 类型由 Attach 时的 flags 决定, 与 info.flags 的 kind 一致:
        // 直接经接口调度, 不再按 flags 分流。
        return it->second.target ? it->second.target->SetFramePeriod(framePeriodNs)
                                 : kPresentNoTarget;
    }

    int Present(uint32_t clientPid, uint32_t surfaceId, GLuint texture,
                uint32_t width, uint32_t height,
                uint64_t drawable, uint32_t serial,
                uint64_t* nextPresentDeadlineNs)
    {
        if (!clientPid || !surfaceId) return kPresentNoTarget;
        const uint64_t surfaceKey =
            (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = surfaces_[surfaceKey];
        // 防御: GL 帧送达 venus (vulkan) target — 错误通道。原按 info.flags
        // 判断, 现改按 target 类型 (kind 一致)。无 target 时无法判断, 先
        // 走 no-target 判定。
        if (entry.target && entry.target->IsVulkan()) return kPresentInvalid;
        entry.info.surfaceKey = surfaceKey;
        entry.info.clientPid = clientPid;
        entry.info.surfaceId = surfaceId;
        entry.info.width = width;
        entry.info.height = height;
        entry.info.serial = serial;
        entry.lastPresentUs = NowUs();
        if (!entry.target) return kPresentNoTarget;
        return entry.target->Present(
            texture, width, height, drawable, serial, nextPresentDeadlineNs);
    }

    int PresentVenus(uint32_t contextId,
                     uintptr_t instance,
                     uintptr_t physicalDevice,
                     uintptr_t device,
                     uintptr_t queue,
                     uint64_t image,
                     uint32_t queueFamily,
                     uint32_t width,
                     uint32_t height,
                     uint32_t format,
                     uint32_t layout,
                     uint32_t clientPid,
                     uint32_t surfaceId,
                     uint32_t serial,
                     uint64_t* nextPresentDeadlineNs,
                     void (*releaseQueue)(void*),
                     void* queueSyncData)
    {
        if (!clientPid || !surfaceId) return kPresentInvalid;
        const uint64_t surfaceKey =
            (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
        std::unique_lock<std::mutex> lock(mutex_);
        auto& entry = surfaces_[surfaceKey];
        // 防御: Vulkan 帧送达 virgl (GL) target — 错误通道。
        if (entry.target && !entry.target->IsVulkan()) return kPresentInvalid;
        entry.info.surfaceKey = surfaceKey;
        entry.info.clientPid = clientPid;
        entry.info.surfaceId = surfaceId;
        entry.info.width = width;
        entry.info.height = height;
        entry.info.serial = serial;
        entry.info.flags |= winehua::virgl_ipc::kSurfaceVulkan;
        entry.lastPresentUs = NowUs();
        const auto targetReady = [this, surfaceKey]() {
            const auto it = surfaces_.find(surfaceKey);
            return it != surfaces_.end() && it->second.target && it->second.target->IsVulkan() &&
                   (it->second.info.flags &
                    winehua::virgl_ipc::kSurfaceAttached);
        };
        if (!targetReady()) {
            if (!entry.missingTargetLogged) {
                entry.missingTargetLogged = true;
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-PRESENT][NCP] target missing key=%{public}llu "
                            "ctx=%{public}u pid=%{public}u surface=%{public}u",
                            static_cast<unsigned long long>(surfaceKey),
                            contextId, clientPid, surfaceId);
            }
            const uint64_t generation = surfaceGenerations_[surfaceKey];
            const auto waitStart = SteadyClock::now();
            targetCondition_.wait_for(
                lock, kVenusTargetAttachTimeout,
                [this, surfaceKey, generation, &targetReady]() {
                    return targetReady() ||
                           surfaceGenerations_[surfaceKey] != generation;
                });
            const uint64_t waitedUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    SteadyClock::now() - waitStart).count());
            if (!targetReady()) {
                OH_LOG_WARN(LOG_APP,
                            "[VENUS-PRESENT][NCP] target wait ended key=%{public}llu "
                            "ctx=%{public}u waited_us=%{public}llu reason=%{public}s",
                            static_cast<unsigned long long>(surfaceKey), contextId,
                            static_cast<unsigned long long>(waitedUs),
                            surfaceGenerations_[surfaceKey] != generation
                                ? "detached" : "timeout");
                return -EAGAIN;
            }
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] target ready key=%{public}llu "
                        "ctx=%{public}u waited_us=%{public}llu",
                        static_cast<unsigned long long>(surfaceKey), contextId,
                        static_cast<unsigned long long>(waitedUs));
        }
        auto readyIt = surfaces_.find(surfaceKey);
        if (readyIt == surfaces_.end() || !readyIt->second.target ||
            !readyIt->second.target->IsVulkan())
            return -EAGAIN;
        return readyIt->second.target->PresentVenus(
            contextId, instance, physicalDevice, device, queue, image,
            queueFamily, width, height, format, layout, serial,
            nextPresentDeadlineNs, releaseQueue, queueSyncData);
    }

    winehua::virgl_ipc::SurfaceQueryReply Query() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        winehua::virgl_ipc::SurfaceQueryReply reply;
        const uint64_t nowUs = NowUs();
        std::vector<const Entry*> candidates;
        candidates.reserve(surfaces_.size());
        for (const auto& [surfaceKey, entry] : surfaces_)
        {
            static_cast<void>(surfaceKey);
            if (!entry.info.surfaceId ||
                (!(entry.info.flags & winehua::virgl_ipc::kSurfaceAttached) &&
                 nowUs - entry.lastPresentUs > 2000000))
                continue;
            candidates.push_back(&entry);
        }

        // unordered_map iteration is deliberately unspecified. Returning that
        // order made the main compositor bind a different live surface after a
        // restart when multiple Wine/Explorer clients were present. Prefer the
        // surface that most recently submitted a frame, with deterministic
        // serial/key tie breakers for startup races.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Entry* a, const Entry* b) {
                      if (a->lastPresentUs != b->lastPresentUs)
                          return a->lastPresentUs > b->lastPresentUs;
                      if (a->info.serial != b->info.serial)
                          return a->info.serial > b->info.serial;
                      return a->info.surfaceKey > b->info.surfaceKey;
                  });
        for (const Entry* entry : candidates)
        {
            if (reply.count == winehua::virgl_ipc::kMaxSurfaces) break;
            reply.surfaces[reply.count++] = entry->info;
        }
        return reply;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [surfaceKey, entry] : surfaces_)
        {
            if (entry.target) {
                entry.target->Detach(surfaceKey);
                if (entry.target->HasVulkanDevice())
                    retiredVenusTargets_.push_back(std::move(entry.target));
            }
            ++surfaceGenerations_[surfaceKey];
        }
        surfaces_.clear();
        targetCondition_.notify_all();
    }

private:
    // 退役一个 venus (IsVulkan) 目标: detach 后若持有 Vk device 则延迟释放
    // (移入 retiredVenusTargets_, 待 device-owner callback), 否则立即释放。
    // virgl (非 vulkan) 目标不在本函数处理 — 其无延迟释放机制, 调用方按需
    // Detach + reset。
    void RetireTargetLocked(uint64_t surfaceKey, std::unique_ptr<PresentTarget>& target)
    {
        if (!target || !target->IsVulkan()) return;
        target->Detach(surfaceKey);
        if (target->HasVulkanDevice())
            retiredVenusTargets_.push_back(std::move(target));
        else
            target.reset();
    }

    struct Entry {
        winehua::virgl_ipc::SurfaceInfo info;
        std::unique_ptr<PresentTarget> target;
        uint64_t lastPresentUs = 0;
        bool missingTargetLogged = false;
    };

    mutable std::mutex mutex_;
    std::condition_variable targetCondition_;
    std::unordered_map<uint64_t, Entry> surfaces_;
    std::unordered_map<uint64_t, uint64_t> surfaceGenerations_;
    // 仅存 venus (has Vk device) 目标; 类型为通用 present_target 接口
    std::vector<std::unique_ptr<PresentTarget>> retiredVenusTargets_;
};

SurfaceQueuePresenterManager g_presenters;

} // namespace

namespace winehua {

int AttachVirglSurfaceTarget(uint64_t surfaceKey, uint64_t framePeriodNs,
                             uint32_t flags, OHNativeWindow* window)
{
    return g_presenters.Attach(surfaceKey, framePeriodNs, flags, window);
}

int DetachVirglSurfaceTarget(uint64_t surfaceKey)
{
    return g_presenters.Detach(surfaceKey);
}

int SetVirglSurfaceFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
{
    return g_presenters.SetFramePeriod(surfaceKey, framePeriodNs);
}

int PresentVirglSurface(uint32_t clientPid, uint32_t surfaceId,
                        uint32_t texture, uint32_t width, uint32_t height,
                        uint64_t drawable, uint32_t serial,
                        uint64_t* nextPresentDeadlineNs)
{
    return g_presenters.Present(
        clientPid, surfaceId, texture, width, height, drawable, serial,
        nextPresentDeadlineNs);
}

int PresentVenusSurface(uint32_t contextId,
                        uintptr_t instance,
                        uintptr_t physicalDevice,
                        uintptr_t device,
                        uintptr_t queue,
                        uint64_t image,
                        uint32_t queueFamily,
                        uint32_t width,
                        uint32_t height,
                        uint32_t format,
                        uint32_t layout,
                        uint32_t clientPid,
                        uint32_t surfaceId,
                        uint32_t serial,
                        uint64_t* nextPresentDeadlineNs,
                        void (*releaseQueue)(void*),
                        void* queueSyncData)
{
    return g_presenters.PresentVenus(
        contextId, instance, physicalDevice, device, queue, image,
        queueFamily, width, height, format, layout, clientPid, surfaceId,
        serial, nextPresentDeadlineNs, releaseQueue, queueSyncData);
}

int PrepareVenusDeviceRelease(uint32_t contextId, uintptr_t device)
{
    return g_presenters.PrepareVenusDeviceRelease(contextId, device);
}

int FinishVenusDeviceRelease(uint32_t contextId, uintptr_t device,
                             int32_t waitResult)
{
    return g_presenters.FinishVenusDeviceRelease(
        contextId, device, waitResult);
}

virgl_ipc::SurfaceQueryReply QueryVirglSurfaces()
{
    return g_presenters.Query();
}

void ResetVirglSurfaces()
{
    g_presenters.Reset();
}

} // namespace winehua
