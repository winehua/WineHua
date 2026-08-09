#include "egl_renderer.h"
#include "graphics_broker.h"
#include "perf_utils.h"
#include "shader_utils.h"
#include "wayland_server.h"
#include "fps_counter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <fcntl.h>
#include <native_vsync/native_vsync.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_EGL"
#include <hilog/log.h>

// -- 共享 EGLDisplay: 整个进程只初始化一次, 避免反复 init/terminate 导致 GPU 驱动竞争 --
static EGLDisplay gSharedDisplay = EGL_NO_DISPLAY;
static std::once_flag gDisplayOnce;

using winehua::PerfClock;
using winehua::PerfNowUs;
using winehua::RendererPerfWindow;

static bool TraceFrameOrder()
{
    const char* trace = std::getenv("VKR_WINEHUA_SHADOW_TRACE");
    return trace && ((!strcmp(trace, "1")) || !strcmp(trace, "present-image-trace"));
}

static void ComposeZeroCopySamplingTransform(const float* nativeTransform,
                                             bool flipY,
                                             float* samplingTransform)
{
    if (!nativeTransform || !samplingTransform) return;
    std::copy(nativeTransform, nativeTransform + 16, samplingTransform);
    if (!flipY) return;
    for (int row = 0; row < 4; ++row) {
        samplingTransform[4 + row] = -nativeTransform[4 + row];
        samplingTransform[12 + row] = nativeTransform[4 + row] + nativeTransform[12 + row];
    }
}

void EglRenderer::OnVSync(long long timestamp, void* data)
{
    static_cast<void>(timestamp);
    auto* renderer = static_cast<EglRenderer*>(data);
    {
        std::lock_guard<std::mutex> lock(renderer->vsyncMutex_);
        ++renderer->vsyncSequence_;
    }
    renderer->vsyncCv_.notify_one();
}

void EglRenderer::OnZeroCopyFrameAvailable(void* data)
{
    auto* renderer = static_cast<EglRenderer*>(data);
    if (!renderer) return;
    renderer->zeroCopyFrameSignals_.fetch_add(1, std::memory_order_relaxed);
    renderer->zeroCopyFrameAvailable_.store(true, std::memory_order_release);
}

EGLDisplay EglRenderer::GetSharedDisplay() {
    std::call_once(gDisplayOnce, []() {
        gSharedDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (gSharedDisplay == EGL_NO_DISPLAY) {
            OH_LOG_ERROR(LOG_APP, "[EGL] eglGetDisplay FAILED");
            return;
        }
        EGLint major, minor;
        if (!eglInitialize(gSharedDisplay, &major, &minor)) {
            OH_LOG_ERROR(LOG_APP, "[EGL] eglInitialize FAILED: 0x%{public}x", eglGetError());
            gSharedDisplay = EGL_NO_DISPLAY;
            return;
        }
        OH_LOG_INFO(LOG_APP, "[EGL] shared display init OK EGL %{public}d.%{public}d", major, minor);
    });
    return gSharedDisplay;
}

using winehua::kFullscreenQuadVS;
using winehua::kFullscreenQuadFS;
using winehua::kZeroCopyExternalFS;
using winehua::CompileShader;

bool EglRenderer::InitZeroCopyConsumer()
{
    if (toplevelId_ == 0 ||
        winehua::GraphicsBroker::GetInstance().GetState().active != winehua::GraphicsBackend::Virgl)
        return false;

    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kFullscreenQuadVS);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kZeroCopyExternalFS);
    zeroCopyProgram_ = glCreateProgram();
    glAttachShader(zeroCopyProgram_, vertex);
    glAttachShader(zeroCopyProgram_, fragment);
    glLinkProgram(zeroCopyProgram_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(zeroCopyProgram_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[1024] = {};
        glGetProgramInfoLog(zeroCopyProgram_, sizeof(log), nullptr, log);
        OH_LOG_WARN(LOG_APP, "[VIRGL-ZC][MAIN] external program link failed: %{public}s", log);
        ShutdownZeroCopyConsumer();
        return false;
    }
    zeroCopyTransformLocation_ = glGetUniformLocation(zeroCopyProgram_, "uTransform");
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] pipeline ready tl=%{public}u", toplevelId_);
    return true;
}

bool EglRenderer::TryAttachZeroCopySurface(uint32_t rendererToplevelId)
{
    if (!zeroCopyProgram_) return false;
    const uint64_t nowUs = PerfNowUs();
    auto& broker = winehua::GraphicsBroker::GetInstance();
    WaylandServer* server = WaylandServer::GetInstance();

    if (zeroCopyRegistered_)
    {
        WaylandServer::ZeroCopyLayerInfo layer;
        if (!server->GetZeroCopyLayerInfo(zeroCopySurfaceKey_, rendererToplevelId,
                                          zeroCopySourceW_, zeroCopySourceH_, layer))
        {
            ReleaseZeroCopyBinding();
        }
        else
        {
            if (zeroCopyLayerX_ != layer.x || zeroCopyLayerY_ != layer.y ||
                zeroCopyLayerW_ != layer.width || zeroCopyLayerH_ != layer.height ||
                zeroCopyFullscreen_ != (layer.fullscreen && server->Policy().RootCompositing()))
                zeroCopyGeometryDirty_ = true;
            zeroCopyLayerX_ = layer.x;
            zeroCopyLayerY_ = layer.y;
            zeroCopyLayerW_ = layer.width;
            zeroCopyLayerH_ = layer.height;
            zeroCopyFullscreen_ = layer.fullscreen && server->Policy().RootCompositing();
            if (zeroCopyFallbackPending_ &&
                layer.shmCommitSerial > zeroCopyFallbackShmSerial_)
            {
                ClearZeroCopyCompositorKey();
                zeroCopyFallbackPending_ = false;
                zeroCopyHasFrame_ = false;
                OH_LOG_WARN(LOG_APP,
                            "[VIRGL-ZC][MAIN] CPU_FALLBACK tl=%{public}u key=%{public}llu "
                            "shm_serial=%{public}llu baseline=%{public}llu",
                            rendererToplevelId,
                            static_cast<unsigned long long>(zeroCopySurfaceKey_),
                            static_cast<unsigned long long>(layer.shmCommitSerial),
                            static_cast<unsigned long long>(zeroCopyFallbackShmSerial_));
            }
        }
    }

    if (nowUs - zeroCopyLastQueryUs_ < 100000) return zeroCopyRegistered_;
    zeroCopyLastQueryUs_ = nowUs;

    std::vector<winehua::ZeroCopySurfaceInfo> surfaces;
    if (!broker.QueryZeroCopySurfaces(surfaces)) return zeroCopyRegistered_;
    if (zeroCopyRegistered_)
    {
        for (const auto& surface : surfaces)
        {
            if (surface.surfaceKey != zeroCopySurfaceKey_) continue;
            if (surface.vulkan != broker.IsVulkanPresentMode()) {
                ReleaseZeroCopyBinding();
                break;
            }
            zeroCopySourceW_ = static_cast<int>(surface.width);
            zeroCopySourceH_ = static_cast<int>(surface.height);
            zeroCopyVulkanSource_ = surface.vulkan;
            return true;
        }
        return true;
    }

    const bool wantVulkanSurface = broker.IsVulkanPresentMode();
    for (const auto& surface : surfaces)
    {
        if (!surface.surfaceKey || surface.attached) continue;
        if (surface.vulkan != wantVulkanSurface) continue;
        WaylandServer::ZeroCopyLayerInfo layer;
        if (!server->GetZeroCopyLayerInfo(surface.surfaceKey, rendererToplevelId,
                                          static_cast<int>(surface.width),
                                          static_cast<int>(surface.height), layer)) continue;

        glGenTextures(1, &zeroCopyTexture_);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, zeroCopyTexture_);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        zeroCopyFrameSignals_.store(0, std::memory_order_relaxed);
        zeroCopyFrameAvailable_.store(false, std::memory_order_release);
        zeroCopyImage_ = OH_NativeImage_Create(zeroCopyTexture_, GL_TEXTURE_EXTERNAL_OES);
        if (!zeroCopyImage_)
        {
            ReleaseZeroCopyBinding();
            continue;
        }
        const int32_t sizeResult = OH_ConsumerSurface_SetDefaultSize(
            zeroCopyImage_, static_cast<int32_t>(surface.width),
            static_cast<int32_t>(surface.height));
        const int32_t usageResult = OH_ConsumerSurface_SetDefaultUsage(
            zeroCopyImage_, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
        const int32_t dropResult = OH_NativeImage_SetDropBufferMode(zeroCopyImage_, true);
        OH_OnFrameAvailableListener listener = {};
        listener.context = this;
        listener.onFrameAvailable = &EglRenderer::OnZeroCopyFrameAvailable;
        if (OH_NativeImage_SetOnFrameAvailableListener(zeroCopyImage_, listener) != 0)
        {
            ReleaseZeroCopyBinding();
            continue;
        }
        zeroCopyListenerSet_ = true;
        zeroCopyProducerWindow_ = OH_NativeImage_AcquireNativeWindow(zeroCopyImage_);
        int32_t queueSize = 0;
        if (zeroCopyProducerWindow_)
            OH_NativeWindow_NativeWindowHandleOpt(
                zeroCopyProducerWindow_, GET_BUFFERQUEUE_SIZE, &queueSize);
        if (!zeroCopyProducerWindow_ ||
            !broker.AttachZeroCopyTarget(
                surface.surfaceKey, zeroCopyProducerWindow_,
                static_cast<uint64_t>(vsyncPeriodNs_.load(std::memory_order_relaxed))))
        {
            ReleaseZeroCopyBinding();
            continue;
        }

        zeroCopySurfaceKey_ = surface.surfaceKey;
        zeroCopyClientPid_ = surface.clientPid;
        zeroCopySurfaceId_ = surface.surfaceId;
        zeroCopySourceW_ = static_cast<int>(surface.width);
        zeroCopySourceH_ = static_cast<int>(surface.height);
        zeroCopyVulkanSource_ = surface.vulkan || broker.IsVulkanPresentMode();
        zeroCopyLayerX_ = layer.x;
        zeroCopyLayerY_ = layer.y;
        zeroCopyLayerW_ = layer.width;
        zeroCopyLayerH_ = layer.height;
        zeroCopyRegistered_ = true;
        zeroCopyGeometryDirty_ = true;
        zeroCopyFallbackPending_ = false;
        zeroCopyFallbackShmSerial_ = layer.shmCommitSerial;
        zeroCopyConsecutiveFailures_ = 0;
        zeroCopyLastTimestamp_ = 0;
        zeroCopyTimestampRegressions_ = 0;
        zeroCopyFrames_ = 0;
        zeroCopyUpdates_ = 0;
        zeroCopyLastConsumedSignal_ = 0;
        zeroCopyCoalescedSignals_ = 0;
        zeroCopyDuplicateTimestamps_ = 0;
        zeroCopyFailures_ = 0;
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] consumer attached tl=%{public}u key=%{public}llu "
                    "pid=%{public}u surface=%{public}u source=%{public}dx%{public}d "
                    "layer=%{public}dx%{public}d+%{public}d,%{public}d queue=%{public}d "
                    "size_ret=%{public}d usage_ret=%{public}d drop_ret=%{public}d",
                    rendererToplevelId,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_),
                    zeroCopyClientPid_, zeroCopySurfaceId_,
                    zeroCopySourceW_, zeroCopySourceH_, zeroCopyLayerW_, zeroCopyLayerH_,
                    zeroCopyLayerX_, zeroCopyLayerY_, queueSize,
                    sizeResult, usageResult, dropResult);
        return true;
    }
    return false;
}

bool EglRenderer::UpdateZeroCopyFrame(int& width, int& height)
{
    if (!zeroCopyRegistered_ || !zeroCopyImage_ ||
        !zeroCopyFrameAvailable_.exchange(false, std::memory_order_acq_rel))
        return false;

    const uint64_t signalCount = zeroCopyFrameSignals_.load(std::memory_order_acquire);
    const uint64_t signalDelta = signalCount >= zeroCopyLastConsumedSignal_
        ? signalCount - zeroCopyLastConsumedSignal_ : 0;
    if (signalDelta > 1) zeroCopyCoalescedSignals_ += signalDelta - 1;
    zeroCopyLastConsumedSignal_ = signalCount;
    ++zeroCopyUpdates_;

    const int32_t updateResult = OH_NativeImage_UpdateSurfaceImage(zeroCopyImage_);
    const int32_t transformResult = updateResult == 0
        ? OH_NativeImage_GetTransformMatrixV2(zeroCopyImage_, zeroCopyTransform_) : -1;
    if (updateResult != 0 || transformResult != 0)
    {
        ++zeroCopyFailures_;
        ++zeroCopyConsecutiveFailures_;
        if (zeroCopyFailures_ == 1 || zeroCopyFailures_ % 60 == 0)
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] update failed tl=%{public}u update=%{public}d "
                        "transform=%{public}d failures=%{public}llu",
                        toplevelId_, updateResult, transformResult,
                        static_cast<unsigned long long>(zeroCopyFailures_));
        if (zeroCopyReadyPublished_ && !zeroCopyFallbackPending_ &&
            zeroCopyConsecutiveFailures_ >= 8)
        {
            WaylandServer::ZeroCopyLayerInfo layer;
            uint32_t rendererToplevelId = toplevelId_;
            WaylandServer* server = WaylandServer::GetInstance();
            if (server->Policy().RootCompositing())
                rendererToplevelId = server->GetDesktopRootToplevelId();
            if (server->GetZeroCopyLayerInfo(zeroCopySurfaceKey_, rendererToplevelId,
                                              zeroCopySourceW_, zeroCopySourceH_, layer))
                zeroCopyFallbackShmSerial_ = layer.shmCommitSerial;
            UnpublishZeroCopyReady(rendererToplevelId);
            zeroCopyFallbackPending_ = true;
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] fallback pending tl=%{public}u key=%{public}llu "
                        "failures=%{public}u shm_baseline=%{public}llu",
                        rendererToplevelId,
                        static_cast<unsigned long long>(zeroCopySurfaceKey_),
                        zeroCopyConsecutiveFailures_,
                        static_cast<unsigned long long>(zeroCopyFallbackShmSerial_));
        }
        return false;
    }

    zeroCopyConsecutiveFailures_ = 0;
    ComposeZeroCopySamplingTransform(zeroCopyTransform_, zeroCopyVulkanSource_,
                                      zeroCopySamplingTransform_);
    const int64_t imageTimestamp = OH_NativeImage_GetTimestamp(zeroCopyImage_);
    const int64_t previousTimestamp = zeroCopyLastTimestamp_;
    int64_t timestampDeltaUs = 0;
    if (imageTimestamp > 0)
    {
        if (previousTimestamp > 0)
        {
            timestampDeltaUs = (imageTimestamp - previousTimestamp) / 1000;
            if (imageTimestamp < previousTimestamp) {
                ++zeroCopyTimestampRegressions_;
                if (zeroCopyTimestampRegressions_ == 1 || zeroCopyTimestampRegressions_ % 60 == 0)
                    OH_LOG_WARN(LOG_APP,
                                "[VIRGL-ZC][MAIN] timestamp regression tl=%{public}u "
                                "current=%{public}lld previous=%{public}lld count=%{public}llu",
                                toplevelId_, static_cast<long long>(imageTimestamp),
                                static_cast<long long>(previousTimestamp),
                                static_cast<unsigned long long>(zeroCopyTimestampRegressions_));
            } else if (imageTimestamp == previousTimestamp) {
                ++zeroCopyDuplicateTimestamps_;
            }
        }
        if (imageTimestamp > zeroCopyLastTimestamp_)
            zeroCopyLastTimestamp_ = imageTimestamp;
    }

    WaylandServer::ZeroCopyLayerInfo layer;
    uint32_t rendererToplevelId = toplevelId_;
    WaylandServer* server = WaylandServer::GetInstance();
    if (server->Policy().RootCompositing()) rendererToplevelId = server->GetDesktopRootToplevelId();
    if (!server->GetZeroCopyLayerInfo(zeroCopySurfaceKey_, rendererToplevelId,
                                      zeroCopySourceW_, zeroCopySourceH_, layer))
    {
        ReleaseZeroCopyBinding();
        return false;
    }
    zeroCopyLayerX_ = layer.x;
    zeroCopyLayerY_ = layer.y;
    zeroCopyLayerW_ = layer.width;
    zeroCopyLayerH_ = layer.height;
    zeroCopyFullscreen_ = layer.fullscreen && server->Policy().RootCompositing();
    width = zeroCopySourceW_;
    height = zeroCopySourceH_;
    zeroCopyHasFrame_ = true;
    if (zeroCopyFallbackPending_)
    {
        zeroCopyFallbackPending_ = false;
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] fallback cancelled by GPU recovery tl=%{public}u key=%{public}llu",
                    rendererToplevelId,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_));
    }
    PublishZeroCopyActive(rendererToplevelId);
    ++zeroCopyFrames_;
    if (TraceFrameOrder() && zeroCopyFrames_ <= 600)
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-ORDER][MAIN] frame=%{public}llu signals=%{public}llu "
                    "updates=%{public}llu signal_delta=%{public}llu coalesced=%{public}llu "
                    "timestamp=%{public}lld timestamp_delta_us=%{public}lld "
                    "timestamp_dup=%{public}llu timestamp_regress=%{public}llu",
                    static_cast<unsigned long long>(zeroCopyFrames_),
                    static_cast<unsigned long long>(zeroCopyFrameSignals_.load()),
                    static_cast<unsigned long long>(zeroCopyUpdates_),
                    static_cast<unsigned long long>(signalDelta),
                    static_cast<unsigned long long>(zeroCopyCoalescedSignals_),
                    static_cast<long long>(imageTimestamp), static_cast<long long>(timestampDeltaUs),
                    static_cast<unsigned long long>(zeroCopyDuplicateTimestamps_),
                    static_cast<unsigned long long>(zeroCopyTimestampRegressions_));
    if (zeroCopyFrames_ == 1 || zeroCopyFrames_ % 120 == 0)
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] frame=%{public}llu tl=%{public}u key=%{public}llu "
                    "source=%{public}dx%{public}d layer=%{public}dx%{public}d+%{public}d,%{public}d "
                    "signals=%{public}llu failures=%{public}llu",
                    static_cast<unsigned long long>(zeroCopyFrames_), toplevelId_,
                    static_cast<unsigned long long>(zeroCopySurfaceKey_), width, height,
                    zeroCopyLayerW_, zeroCopyLayerH_, zeroCopyLayerX_, zeroCopyLayerY_,
                    static_cast<unsigned long long>(zeroCopyFrameSignals_.load()),
                    static_cast<unsigned long long>(zeroCopyFailures_));
    return width > 0 && height > 0;
}

// -- ZC 状态发布点收敛 (阶段 3) --
// 三处状态 (compositor zeroCopySurfaceKeys_ / broker ready marker /
// renderer 内部 zeroCopyReadyPublished_) 的全部更新收敛到这三个方法,
// 每个幂等。时序是协议设计, 不可合并: 发布先 compositor key 后 ready
// (先让合成跳过, 再通知 guest 走 ZC); fallback 分两步 — 先撤 ready
// (guest 立即切 SHM), 等 shmCommitSerial 越过基线 (新 SHM 帧已到) 再撤
// compositor key (恢复合成), 避免合成到 ZC 前的旧 SHM 帧。
void EglRenderer::PublishZeroCopyActive(uint32_t rendererToplevelId)
{
    if (zeroCopyReadyPublished_) return;
    WaylandServer::GetInstance()->SetSurfaceZeroCopy(zeroCopySurfaceKey_, true);
    winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(
        zeroCopySurfaceKey_, true);
    zeroCopyReadyPublished_ = true;
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] GPU_ACTIVE tl=%{public}u key=%{public}llu",
                rendererToplevelId,
                static_cast<unsigned long long>(zeroCopySurfaceKey_));
}

void EglRenderer::UnpublishZeroCopyReady(uint32_t rendererToplevelId)
{
    if (!zeroCopyReadyPublished_) return;
    winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(
        zeroCopySurfaceKey_, false);
    zeroCopyReadyPublished_ = false;
    OH_LOG_WARN(LOG_APP,
                "[VIRGL-ZC][MAIN] ready revoked tl=%{public}u key=%{public}llu",
                rendererToplevelId,
                static_cast<unsigned long long>(zeroCopySurfaceKey_));
}

void EglRenderer::ClearZeroCopyCompositorKey()
{
    // SetSurfaceZeroCopy 内部有 surfaceKey 检查, erase 不存在的 key 是
    // no-op — 天然幂等
    WaylandServer::GetInstance()->SetSurfaceZeroCopy(zeroCopySurfaceKey_, false);
}

void EglRenderer::ReleaseZeroCopyBinding()
{
    // Teardown logs below distinguish SurfaceQueue ownership failures from rendering failures.
    const uint64_t surfaceKey = zeroCopySurfaceKey_;
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] release begin tl=%{public}u key=%{public}llu "
                "registered=%{public}d ready=%{public}d listener=%{public}d image=%{public}p",
                toplevelId_, static_cast<unsigned long long>(surfaceKey),
                zeroCopyRegistered_, zeroCopyReadyPublished_, zeroCopyListenerSet_,
                zeroCopyImage_);
    if (zeroCopySurfaceKey_)
    {
        // 幂等: 未发布过 (attach 早退/从未 GPU_ACTIVE) 时两个方法都是 no-op
        UnpublishZeroCopyReady(toplevelId_);
        ClearZeroCopyCompositorKey();
    }
    zeroCopyFallbackPending_ = false;
    if (zeroCopyRegistered_) {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release detach begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        winehua::GraphicsBroker::GetInstance().DetachZeroCopyTarget(zeroCopySurfaceKey_);
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release detach end key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
    }
    zeroCopyRegistered_ = false;
    if (zeroCopyImage_ && zeroCopyListenerSet_) {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release listener-unset begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        const int32_t unsetResult = OH_NativeImage_UnsetOnFrameAvailableListener(zeroCopyImage_);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] release listener-unset end key=%{public}llu result=%{public}d",
                    static_cast<unsigned long long>(surfaceKey), unsetResult);
    }
    zeroCopyListenerSet_ = false;
    zeroCopyProducerWindow_ = nullptr;
    if (zeroCopyImage_) {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release image-destroy begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        OH_NativeImage_Destroy(&zeroCopyImage_);
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release image-destroy end key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
    }
    if (zeroCopyTexture_)
    {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release texture-delete begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        glDeleteTextures(1, &zeroCopyTexture_);
        zeroCopyTexture_ = 0;
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release texture-delete end key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
    }
    zeroCopyFrameAvailable_.store(false, std::memory_order_release);
    zeroCopyHasFrame_ = false;
    zeroCopyGeometryDirty_ = false;
    zeroCopyConsecutiveFailures_ = 0;
    zeroCopyFallbackShmSerial_ = 0;
    zeroCopyLastTimestamp_ = 0;
    zeroCopyTimestampRegressions_ = 0;
    zeroCopyUpdates_ = 0;
    zeroCopyLastConsumedSignal_ = 0;
    zeroCopyCoalescedSignals_ = 0;
    zeroCopyDuplicateTimestamps_ = 0;
    zeroCopySurfaceKey_ = 0;
    zeroCopyClientPid_ = 0;
    zeroCopySurfaceId_ = 0;
    zeroCopySourceW_ = 0;
    zeroCopySourceH_ = 0;
    zeroCopyVulkanSource_ = false;
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release complete tl=%{public}u key=%{public}llu",
                toplevelId_, static_cast<unsigned long long>(surfaceKey));
}

void EglRenderer::ShutdownZeroCopyConsumer()
{
    ReleaseZeroCopyBinding();
    if (zeroCopyProgram_)
    {
        glDeleteProgram(zeroCopyProgram_);
        zeroCopyProgram_ = 0;
    }
}

bool EglRenderer::Init(OHNativeWindow* window, int w, int h) {
    window_ = window;
    width_ = w;
    height_ = h;

    OH_LOG_INFO(LOG_APP, "[EGL] Init tl=%{public}u req=%{public}dx%{public}d", toplevelId_, w, h);

    // 1. 使用共享 EGLDisplay (全进程只 init 一次)
    display_ = GetSharedDisplay();
    if (display_ == EGL_NO_DISPLAY) {
        OH_LOG_ERROR(LOG_APP, "[EGL] shared display unavailable tl=%{public}u", toplevelId_);
        return false;
    }

    EGLConfig cfg;
    EGLint nCfg;
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    eglChooseConfig(display_, attrs, &cfg, 1, &nCfg);

    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctxAttrs);

    // 异型窗口 (layered/shaped): 确保 native window buffer 带 alpha 通道,
    // 否则 per-pixel alpha 在 buffer 层就被丢弃 (默认可能是 RGBX)
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);

    // OHOS: EGLNativeWindowType = OHNativeWindow* (cast to unsigned long)
    surface_ = eglCreateWindowSurface(display_, cfg,
                                       reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed tl=%{public}u: 0x%{public}x", toplevelId_, eglGetError());
        return false;
    }
    {
        EGLint sw = 0, sh = 0, alphaBits = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &sw);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &sh);
        eglQuerySurface(display_, surface_, EGL_ALPHA_SIZE, &alphaBits);
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u eglSurface %{public}dx%{public}d alphaBits=%{public}d",
                    toplevelId_, sw, sh, alphaBits);
    }

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Init done, render thread started OK", toplevelId_);
    return true;
}

void EglRenderer::RenderLoop() {
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglMakeCurrent failed: 0x%{public}x", eglGetError());
        return;
    }

    // NativeVSync owns frame scheduling. Disable EGL's independent swap pacing
    // so a frame does not wait once for VSync and again inside eglSwapBuffers.
    const bool swapIntervalDisabled = eglSwapInterval(display_, 0) == EGL_TRUE;
    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u eglSwapInterval(0)=%{public}s",
                toplevelId_, swapIntervalDisabled ? "OK" : "FAIL");

    // 2. 着色器
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kFullscreenQuadVS);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFullscreenQuadFS);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    // 3. 全屏 quad VBO
    float quad[] = {
        -1,-1, 0,1,   1,-1, 1,1,   -1, 1, 0,0,
         1,-1, 1,1,   1, 1, 1,0,   -1, 1, 0,0,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenBuffers(1, &occluderVbo_);

    // 4. 纹理 (初始空)
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const bool zeroCopyReady = InitZeroCopyConsumer();
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] tl=%{public}u path=%{public}s",
                toplevelId_, zeroCopyReady ? "SURFACE_QUEUE" : "SHM_FALLBACK");

    // 5. 渲染循环: 跟随硬件 VSync, 每次只取最新的 toplevel 帧
    FpsCounter fps("render");
    std::vector<uint8_t> px;
    int fw = 0, fh = 0;
    int loopCount = 0;
    bool firstFrameLogged = false;
    bool rendered = false;  // 首帧已渲染后, 无新帧时跳过 GPU 绘制
    RendererPerfWindow perf;

    static constexpr long long kFallbackPeriodNs = 16666667;
    static constexpr auto kVSyncTimeout = std::chrono::milliseconds(100);
    const char vsyncName[] = "WineHuaRenderer";
    OH_NativeVSync* nativeVsync = OH_NativeVSync_Create(vsyncName, sizeof(vsyncName) - 1);
    if (nativeVsync) {
        OH_NativeVSync_ExpectedRateRange expectedRate = {60, 120, 120};
        const int rateResult = OH_NativeVSync_SetExpectedFrameRateRange(
            nativeVsync, &expectedRate);
        OH_LOG_INFO(LOG_APP,
                    "[MW-RNDR] tl=%{public}u request frame rate min=%{public}d "
                    "max=%{public}d expected=%{public}d result=%{public}d",
                    toplevelId_, expectedRate.min, expectedRate.max,
                    expectedRate.expected, rateResult);
    }
    long long vsyncPeriodNs = vsyncPeriodNs_.load(std::memory_order_relaxed);
    long long loggedPeriodNs = 0;
    unsigned int vsyncFailures = 0;
    auto fallbackDeadline = PerfClock::now();

    auto waitForFrameTick = [&]() -> bool {
        if (!running_) return false;

        if (nativeVsync) {
            uint64_t requestedSequence;
            {
                std::lock_guard<std::mutex> lock(vsyncMutex_);
                requestedSequence = vsyncSequence_;
            }

            const int requestResult = OH_NativeVSync_RequestFrame(
                nativeVsync, &EglRenderer::OnVSync, this);
            if (requestResult == 0) {
                std::unique_lock<std::mutex> lock(vsyncMutex_);
                const bool signaled = vsyncCv_.wait_for(lock, kVSyncTimeout, [&]() {
                    return !running_ || vsyncSequence_ != requestedSequence;
                });
                lock.unlock();

                if (!running_) return false;
                if (signaled) {
                    long long period = 0;
                    if (OH_NativeVSync_GetPeriod(nativeVsync, &period) == 0 && period > 0) {
                        vsyncPeriodNs = period;
                        const long long previousPeriod =
                            vsyncPeriodNs_.load(std::memory_order_relaxed);
                        const long long pacingDelta = period > previousPeriod
                            ? period - previousPeriod : previousPeriod - period;
                        if (pacingDelta >= 500000) {
                            vsyncPeriodNs_.store(period, std::memory_order_relaxed);
                            if (zeroCopySurfaceKey_)
                                winehua::GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(
                                    zeroCopySurfaceKey_, static_cast<uint64_t>(period));
                        }
                        const long long periodDelta = period > loggedPeriodNs
                            ? period - loggedPeriodNs : loggedPeriodNs - period;
                        if (loggedPeriodNs == 0 || periodDelta >= 500000) {
                            const double refreshRate = 1000000000.0 / static_cast<double>(period);
                            OH_LOG_INFO(LOG_APP,
                                        "[MW-RNDR] tl=%{public}u NativeVSync period=%{public}lldns "
                                        "rate=%{public}.2fHz",
                                        toplevelId_, period, refreshRate);
                            loggedPeriodNs = period;
                        }
                    }
                    vsyncFailures = 0;
                    fallbackDeadline = PerfClock::now();
                    return true;
                }
            }

            ++vsyncFailures;
            if (vsyncFailures == 1 || vsyncFailures % 120 == 0) {
                OH_LOG_WARN(LOG_APP,
                            "[MW-RNDR] tl=%{public}u NativeVSync unavailable "
                            "request=%{public}d failures=%{public}u, using deadline fallback",
                            toplevelId_, requestResult, vsyncFailures);
            }
        }

        const auto period = std::chrono::nanoseconds(
            vsyncPeriodNs > 0 ? vsyncPeriodNs : kFallbackPeriodNs);
        const auto now = PerfClock::now();
        fallbackDeadline += period;
        if (fallbackDeadline <= now || fallbackDeadline - now > period * 2)
            fallbackDeadline = now + period;
        std::this_thread::sleep_until(fallbackDeadline);
        return running_;
    };

    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u render loop started pacing=%{public}s",
                toplevelId_, nativeVsync ? "NativeVSync" : "deadline-60Hz");

    while (running_) {
        const uint64_t frameStartedUs = PerfNowUs();
        const uint64_t takeStartedUs = frameStartedUs;
        uint64_t uploadUs = 0;
        bool haveFrame = false;
        bool cpuFrame = false;
        bool zeroCopyFrame = false;
        int zeroCopyWidth = 0;
        int zeroCopyHeight = 0;
        uint32_t useToplevel = toplevelId_;
        WaylandServer* ws = WaylandServer::GetInstance();
        // Desktop mode: root toplevel may be recreated, always use current ID
        if (ws->Policy().RootCompositing()) useToplevel = ws->GetDesktopRootToplevelId();
        TryAttachZeroCopySurface(useToplevel);
        const bool zeroCopyGeometryFrame = zeroCopyGeometryDirty_;
        zeroCopyGeometryDirty_ = false;
        zeroCopyFrame = UpdateZeroCopyFrame(zeroCopyWidth, zeroCopyHeight);
        if (useToplevel != 0) {
            cpuFrame = ws->TakeToplevelFrame(useToplevel, px, fw, fh);
            if (cpuFrame) {
                // ARGB8888 帧 (layered/shaped 异型窗口) 透传 alpha; XRGB 强制不透明
                frameArgb_ = (ws->GetToplevelShmFormat(useToplevel) == 0);
            }
        } else {
            cpuFrame = ws->TakeFrame(px, fw, fh);
        }
        haveFrame = cpuFrame || zeroCopyFrame || zeroCopyGeometryFrame;
        const uint64_t takeUs = PerfNowUs() - takeStartedUs;

        if (cpuFrame && fw > 0 && fh > 0) {
            const uint64_t uploadStartedUs = PerfNowUs();
            // 存储帧尺寸供输入坐标转换
            frameW_ = fw;
            frameH_ = fh;
            if (!firstFrameLogged) {
                OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u  FIRST FRAME %{public}dx%{public}d px=%{public}zu",
                            useToplevel, fw, fh, px.size());
                firstFrameLogged = true;
            }
            glBindTexture(GL_TEXTURE_2D, texture_);
            int rowLen = (int)px.size() / fh / 4;
            if (rowLen != fw) {
                OH_LOG_WARN(LOG_APP, "[MW-RNDR] UNPACK_ROW_LENGTH rowLen=%{public}d fw=%{public}d px=%{public}zu fh=%{public}d",
                            rowLen, fw, px.size(), fh);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);
            }
            // 首帧/尺寸变化: glTexImage2D (分配 GPU 内存)
            // 同尺寸: glTexSubImage2D (复用, 仅 memcpy → GPU)
            if (fw != texW_ || fh != texH_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                texW_ = fw; texH_ = fh;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh,
                                GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            }
            if (rowLen != fw) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            uploadUs = PerfNowUs() - uploadStartedUs;
            rendered = true;
        }
        if (zeroCopyFrame && !firstFrameLogged) {
            OH_LOG_INFO(LOG_APP,
                        "[MW-RNDR] tl=%{public}u FIRST ZERO-COPY FRAME %{public}dx%{public}d",
                        useToplevel, zeroCopyWidth, zeroCopyHeight);
            firstFrameLogged = true;
        }

        // 无新帧且已渲染过首帧 → 跳过 GPU 绘制, 静态桌面节省 GPU 功耗。
        // 例外: EGL surface 尺寸变了 (最小化还原/窗口 resize) 必须用当前纹理
        // 重新 letterbox 上屏 — 否则最后一帧可能画在旧尺寸 buffer 上
        // (ForceToplevelRedraw 的 dirty 与 buffer 异步切换存在竞争窗口),
        // 静止窗口再无新帧触发重绘, 系统把旧 buffer 拉伸显示导致缩放错误
        if (!haveFrame && rendered) {
            EGLint curW = 0, curH = 0;
            eglQuerySurface(display_, surface_, EGL_WIDTH, &curW);
            eglQuerySurface(display_, surface_, EGL_HEIGHT, &curH);
            if (curW == width_ && curH == height_) {
                loopCount++;
                ++skipFrames_;   // 诊断: 统计跳过 swap 的帧数
                if (!waitForFrameTick()) break;
                continue;
            }
            OH_LOG_INFO(LOG_APP,
                        "[MW-RNDR] tl=%{public}u surface %{public}dx%{public}d -> %{public}dx%{public}d with no new frame, re-letterbox",
                        useToplevel, width_, height_, curW, curH);
        }

        // 获取 EGL surface 实际大小
        EGLint surfW = 0, surfH = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &surfW);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfH);
        if (surfW > 0 && surfH > 0) {
            width_ = surfW;
            height_ = surfH;
        }

        // Letterbox 视口: 保持 Wine 帧宽高比, 居中渲染, 左右或上下黑边。
        // 几何统一由 ComputeFitRect 计算 (与 desktop 合成/输入命中同源;
        // 历史实现此处独立手写, 截断取整与合成的 lround 不一致曾有 1px 偏差)
        if (ComputeFitRect(width_, height_, frameW_, frameH_, letterbox_)) {
            glViewport(letterbox_.offX, letterbox_.offY, letterbox_.dstW, letterbox_.dstH);
        } else {
            letterbox_ = FitRect{};
            glViewport(0, 0, width_, height_);
        }

        // 诊断: 前10帧详细打印 surface -> frame -> viewport 完整映射
        if (loopCount < 10) {
            int barTop = letterbox_.offY;
            int barBot = height_ - letterbox_.offY - letterbox_.dstH;
            int barLeft = letterbox_.offX;
            int barRight = width_ - letterbox_.offX - letterbox_.dstW;
            float sA = (float)width_ / height_;
            float fA = frameW_ > 0 && frameH_ > 0 ? (float)frameW_ / frameH_ : 0;
            OH_LOG_INFO(LOG_APP, "[MW-RNDR] diag#%{public}d tl=%{public}u surface=%{public}dx%{public}d(asp=%{public}.2f) frame=%{public}dx%{public}d(asp=%{public}.2f) vp=%{public}dx%{public}d+%{public}d,%{public}d bar=(L%{public}d R%{public}d T%{public}d B%{public}d)",
                        loopCount, useToplevel,
                        width_, height_, sA, frameW_, frameH_, fA,
                        letterbox_.dstW, letterbox_.dstH, letterbox_.offX, letterbox_.offY,
                        barLeft, barRight, barTop, barBot);
        }

        // surface 变化时打印 XComponent → Wine 尺寸映射 (与 ArkTS MW-RESIZE 共用关键字)
        if ((width_ != lastLoggedW_ || height_ != lastLoggedH_) && loopCount >= 10) {
            lastLoggedW_ = width_;
            lastLoggedH_ = height_;
            OH_LOG_INFO(LOG_APP, "[MW-RESIZE] tl=%{public}u surface=%{public}dx%{public}d frame=%{public}dx%{public}d",
                        useToplevel, width_, height_, frameW_, frameH_);
        }
        // ARGB 窗口清透明底 (letterbox 黑边/未覆盖区域也要能透过),
        // 普通窗口清不透明黑底
        if (frameArgb_) glClearColor(0, 0, 0, 0);
        else glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glActiveTexture(GL_TEXTURE0);

        if (rendered) {
            glViewport(letterbox_.offX, letterbox_.offY, letterbox_.dstW, letterbox_.dstH);
            glUseProgram(program_);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
            glUniform1f(glGetUniformLocation(program_, "uForceOpaque"), frameArgb_ ? 0.0f : 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        if (zeroCopyHasFrame_ && zeroCopyRegistered_ && frameW_ > 0 && frameH_ > 0 &&
            zeroCopyLayerW_ > 0 && zeroCopyLayerH_ > 0) {
            int layerViewportX, layerViewportY, layerViewportW, layerViewportH;
            if (zeroCopyFullscreen_) {
                // ZC 游戏全屏: 层内容保比例缩放进桌面帧的显示区, 而非按帧比例
                // 映射 — 全屏后 buffer 被 Wine 扩到输出尺寸, 但层几何仍是游戏
                // 内部分辨率, 直接映射会把画面缩到左上角一块。CPU 侧整帧已填黑
                // (TakeToplevelFrame ZC 分支), 这里的 letterbox 与 SHM 全屏同效
                FitRect zcFit;
                if (ComputeFitRect(letterbox_.dstW, letterbox_.dstH,
                                   zeroCopyLayerW_, zeroCopyLayerH_, zcFit)) {
                    layerViewportX = letterbox_.offX + zcFit.offX;
                    layerViewportY = letterbox_.offY + zcFit.offY;
                    layerViewportW = zcFit.dstW;
                    layerViewportH = zcFit.dstH;
                } else {
                    layerViewportX = letterbox_.offX;
                    layerViewportY = letterbox_.offY;
                    layerViewportW = letterbox_.dstW;
                    layerViewportH = letterbox_.dstH;
                }
            } else {
                // 帧内坐标 → surface 视口: 与 letterbox 同一映射 (GL 坐标系 Y 向上, 翻转)
                layerViewportX = FitMapDisplayX(letterbox_, zeroCopyLayerX_);
                layerViewportY = FitMapDisplayY(letterbox_, frameH_ - zeroCopyLayerY_ - zeroCopyLayerH_);
                layerViewportW = std::max(1, FitSizeDisplayW(letterbox_, zeroCopyLayerW_));
                layerViewportH = std::max(1, FitSizeDisplayH(letterbox_, zeroCopyLayerH_));
            }
            glViewport(layerViewportX, layerViewportY, layerViewportW, layerViewportH);
            glUseProgram(zeroCopyProgram_);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, zeroCopyTexture_);
            glUniform1i(glGetUniformLocation(zeroCopyProgram_, "uTex"), 0);
            glUniformMatrix4fv(zeroCopyTransformLocation_, 1, GL_FALSE,
                               zeroCopySamplingTransform_);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Desktop 模式 z-order 修复: GL overlay 不参与 CPU 合成的层序,
            // 画完 overlay 后, 把压在 GL 窗口之上的区域 (z-order 更高的窗口/
            // 任务栏/popup 菜单) 用桌面纹理对应子区域重绘回来。桌面纹理里这些
            // 区域已是 CPU 合成好的最终像素, 重绘即恢复正确层序。
            // 本 context 从不开启 GL_BLEND: 重绘就是不透明覆盖, 无需混合,
            // 也不要加混合 —— 目标就是盖住 overlay, 不是与它融合。
            // PC 模式不需要: GL 内容画在各自窗口内, 层序由系统合成器保证。
            // 阶段 2: 全屏 ZC 也走上层覆盖 — GetZeroCopyOccluders 只返回
            // z-order 高于本层的窗口区域, 无上层窗口时结果为空 (无遮挡,
            // 行为不变); 双 GL 实例互叠 (另一窗口被连带标全屏) 时上层窗口
            // 被贴回, 双实例 bug 由此修复 (见 COMPOSITOR_UNIFICATION §5 阶段 2)
            if (ws->Policy().RootCompositing() && rendered) {
                // 32 上限: 遮挡源 = 上层窗口 + popup 层, 真实场景个位数;
                // 超出的部分不重绘 (该区域 GL 内容会透出), 比动态扩容简单且够用
                static constexpr int kMaxOccluders = 32;
                WaylandServer::ZeroCopyOccluderRect occluders[kMaxOccluders];
                const int occluderCount = ws->GetZeroCopyOccluders(
                    zeroCopySurfaceKey_, useToplevel, occluders, kMaxOccluders);
                if (occluderCount > 0) {
                    glUseProgram(program_);
                    glBindTexture(GL_TEXTURE_2D, texture_);
                    glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
                    glUniform1f(glGetUniformLocation(program_, "uForceOpaque"),
                                frameArgb_ ? 0.0f : 1.0f);
                    glBindBuffer(GL_ARRAY_BUFFER, occluderVbo_);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
                    glEnableVertexAttribArray(1);
                    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
                    for (int i = 0; i < occluderCount; ++i) {
                        const auto& r = occluders[i];
                        // 桌面帧像素坐标 → surface 视口 (与 layer 同一 letterbox 映射)
                        const int vx = FitMapDisplayX(letterbox_, r.x);
                        const int vy = FitMapDisplayY(letterbox_, frameH_ - r.y - r.h);
                        const int vw = std::max(1, FitSizeDisplayW(letterbox_, r.w));
                        const int vh = std::max(1, FitSizeDisplayH(letterbox_, r.h));
                        // 桌面纹理 UV 子区域 (纹理第 0 行 = 帧顶部, v 无需翻转)
                        const float u0 = static_cast<float>(r.x) / frameW_;
                        const float u1 = static_cast<float>(r.x + r.w) / frameW_;
                        const float v0 = static_cast<float>(r.y) / frameH_;
                        const float v1 = static_cast<float>(r.y + r.h) / frameH_;
                        const float rquad[] = {
                            -1,-1, u0,v1,   1,-1, u1,v1,   -1,1, u0,v0,
                             1,-1, u1,v1,    1,1, u1,v0,   -1,1, u0,v0,
                        };
                        glBufferData(GL_ARRAY_BUFFER, sizeof(rquad), rquad, GL_DYNAMIC_DRAW);
                        glViewport(vx, vy, vw, vh);
                        glDrawArrays(GL_TRIANGLES, 0, 6);
                    }
                }
            }
        }

        const uint64_t swapStartedUs = PerfNowUs();
        const bool swapOk = eglSwapBuffers(display_, surface_) == EGL_TRUE;
        const uint64_t frameEndedUs = PerfNowUs();
        if (haveFrame) {
            // 诊断: 每帧有帧 swap 都打印 — 对齐合成(MW-TAKE)时刻与上屏(swap)时刻,
            // skip 累计 = 自上次上屏以来跳过多少次无帧循环 (帧被延迟多久)
            OH_LOG_INFO(LOG_APP,
                        "[MW-SWAP] tl=%{public}u loop=%{public}llu f=%{public}d/%{public}d/%{public}d skip=%{public}llu take=%{public}lluus swap=%{public}lluus",
                        useToplevel, static_cast<unsigned long long>(loopCount),
                        cpuFrame ? 1 : 0, zeroCopyFrame ? 1 : 0, zeroCopyGeometryFrame ? 1 : 0,
                        static_cast<unsigned long long>(skipFrames_), takeUs,
                        frameEndedUs - swapStartedUs);
            skipFrames_ = 0;
            perf.Add(useToplevel, takeUs, uploadUs, frameEndedUs - swapStartedUs,
                     frameEndedUs - frameStartedUs, cpuFrame ? px.size() : 0, swapOk);
        }
        fps.Tick();
        loopCount++;
        if (!waitForFrameTick()) break;
    }

    ShutdownZeroCopyConsumer();
    if (nativeVsync) OH_NativeVSync_Destroy(nativeVsync);
}

void EglRenderer::Shutdown() {
    running_ = false;
    vsyncCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        // 每个 renderer 独立 EGLContext, 各自销毁
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        // 不调 eglTerminate: 共享 display 由进程生命周期管理
        // 避免反复 init/terminate 导致 GPU 驱动竞争, 偶发性 SIGSEGV
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Shutdown OK (display retained)", toplevelId_);
    }
    // surfaceId 创建的 native window 在这里销毁 (EglRenderer 持有 window_ 指针)
    if (window_) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u native window destroyed", toplevelId_);
    }
}
