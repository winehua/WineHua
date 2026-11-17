#include "egl_renderer.h"
#include "graphics_broker.h"
#include "gl_capability_probe.h"
#include "display_cadence.h"
#include "common/perf_utils.h"
#include "shader_utils.h"
#include "compositor/toplevel/desktop_compositor.h"  // DesktopCompositor (6A 构造注入: 取帧/ZC 直连)
#include "common/fps_counter.h"
#include "common/frame_loop_diagnostics.h"
#include "direct/direct_desktop_compositor.h"
#include "direct/direct_vulkan_desktop_compositor.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
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
static std::atomic<uint64_t> gAcceptedPresents{0}, gAcceptedGpuPresents{0};
uint64_t GetEglAcceptedPresents() { return gAcceptedPresents.load(std::memory_order_relaxed); }
uint64_t GetEglAcceptedGpuPresents() { return gAcceptedGpuPresents.load(std::memory_order_relaxed); }

using winehua::PerfClock;
using winehua::PerfNowUs;
using winehua::RendererPerfWindow;
using winehua::FrameTraceEnabled;

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

EglRenderer::EglRenderer(DesktopCompositor& compositor) : compositor_(compositor) {}
EglRenderer::~EglRenderer() { Shutdown(); }

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
    auto* state = static_cast<ZeroCopyConsumer*>(data);
    if (!state) return;
    auto& consumer = *state;
    const uint64_t nowUs = PerfNowUs();
    consumer.lastSignalUs.store(nowUs, std::memory_order_relaxed);
    consumer.frameSignals.fetch_add(1, std::memory_order_relaxed);
    consumer.frameAvailable.store(true, std::memory_order_release);
    consumer.renderer->compositor_.zc().NoteProducerPresent(consumer.surfaceKey, nowUs);
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
    if (toplevelId_.load(std::memory_order_relaxed) == 0 ||
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
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] pipeline ready tl=%{public}u", toplevelId_.load(std::memory_order_relaxed));
    return true;
}

bool EglRenderer::TryAttachZeroCopySurface(uint32_t rendererToplevelId)
{
    if (!zeroCopyProgram_) return false;
    const uint64_t nowUs = PerfNowUs();
    auto& broker = winehua::GraphicsBroker::GetInstance();
    for (auto it = zeroCopyConsumers_.begin(); it != zeroCopyConsumers_.end();) {
        auto& consumer = **it;
        ZeroCopyLayerInfo layer;
        if (!consumer.registered || !compositor_.GetZeroCopyLayerInfo(
                consumer.surfaceKey, rendererToplevelId, consumer.sourceW, consumer.sourceH, layer) ||
            consumer.layer.bindingGeneration != layer.bindingGeneration) {
            ReleaseZeroCopyBinding(consumer);
            it = zeroCopyConsumers_.erase(it);
            zeroCopySceneDirty_ = true;
            continue;
        }
        consumer.geometryDirty = consumer.layerX != layer.x || consumer.layerY != layer.y ||
            consumer.layerW != layer.width || consumer.layerH != layer.height ||
            consumer.fullscreen != (layer.fullscreen && RootCompositing());
        consumer.layerX = layer.x; consumer.layerY = layer.y;
        consumer.layerW = layer.width; consumer.layerH = layer.height;
        consumer.fullscreen = layer.fullscreen && RootCompositing();
        consumer.layer = layer;
        if (compositor_.zc().ConfirmFallback(consumer.surfaceKey, layer.shmCommitSerial)) {
            consumer.hasFrame = false;
            zeroCopySceneDirty_ = true;
        }
        ++it;
    }
    if (nowUs - zeroCopyLastQueryUs_ < 100000) return !zeroCopyConsumers_.empty();
    zeroCopyLastQueryUs_ = nowUs;
    std::vector<winehua::ZeroCopySurfaceInfo> surfaces;
    if (!broker.QueryZeroCopySurfaces(surfaces)) return !zeroCopyConsumers_.empty();
    for (const auto& surface : surfaces) {
        const auto found = std::find_if(zeroCopyConsumers_.begin(), zeroCopyConsumers_.end(),
            [&](const auto& state) { return state->surfaceKey == surface.surfaceKey; });
        if (found != zeroCopyConsumers_.end()) {
            auto& consumer = **found;
            if (consumer.vulkanSource == surface.vulkan) {
                consumer.sourceW = static_cast<int>(surface.width);
                consumer.sourceH = static_cast<int>(surface.height);
                continue;
            }
            ReleaseZeroCopyBinding(consumer);
            zeroCopyConsumers_.erase(found);
            zeroCopySceneDirty_ = true;
        }
        if (!surface.surfaceKey || surface.attached) continue;
        ZeroCopyLayerInfo layer;
        if (!compositor_.GetZeroCopyLayerInfo(surface.surfaceKey, rendererToplevelId,
                static_cast<int>(surface.width), static_cast<int>(surface.height), layer)) continue;
        auto state = std::make_unique<ZeroCopyConsumer>();
        auto& consumer = *state;
        consumer.renderer = this;
        consumer.surfaceKey = surface.surfaceKey;
        glGenTextures(1, &consumer.texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, consumer.texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        consumer.frameSignals.store(0, std::memory_order_relaxed);
        consumer.frameAvailable.store(false, std::memory_order_release);
        consumer.image = OH_NativeImage_Create(consumer.texture, GL_TEXTURE_EXTERNAL_OES);
        if (!consumer.image)
        {
            ReleaseZeroCopyBinding(consumer);
            continue;
        }
        const int32_t sizeResult = OH_ConsumerSurface_SetDefaultSize(
            consumer.image, static_cast<int32_t>(surface.width),
            static_cast<int32_t>(surface.height));
        const int32_t usageResult = OH_ConsumerSurface_SetDefaultUsage(
            consumer.image, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
        const int32_t dropResult = OH_NativeImage_SetDropBufferMode(consumer.image, true);
        OH_OnFrameAvailableListener listener = {};
        listener.context = state.get();
        listener.onFrameAvailable = &EglRenderer::OnZeroCopyFrameAvailable;
        if (OH_NativeImage_SetOnFrameAvailableListener(consumer.image, listener) != 0)
        {
            ReleaseZeroCopyBinding(consumer);
            continue;
        }
        consumer.listenerSet = true;
        consumer.producerWindow = OH_NativeImage_AcquireNativeWindow(consumer.image);
        int32_t queueSize = 0;
        if (consumer.producerWindow)
            OH_NativeWindow_NativeWindowHandleOpt(
                consumer.producerWindow, GET_BUFFERQUEUE_SIZE, &queueSize);
        if (!consumer.producerWindow ||
            !broker.AttachZeroCopyTarget(
                surface.surfaceKey, consumer.producerWindow,
                renderPaused_.load(std::memory_order_acquire) ? 100000000ull :
                    static_cast<uint64_t>(vsyncPeriodNs_.load(std::memory_order_relaxed)),
                surface.vulkan))
        {
            ReleaseZeroCopyBinding(consumer);
            continue;
        }

        consumer.clientPid = surface.clientPid;
        consumer.surfaceId = surface.surfaceId;
        consumer.attachUs = PerfNowUs();
        consumer.sourceW = static_cast<int>(surface.width);
        consumer.sourceH = static_cast<int>(surface.height);
        consumer.vulkanSource = surface.vulkan;
        consumer.layer = layer;
        consumer.layerX = layer.x;
        consumer.layerY = layer.y;
        consumer.layerW = layer.width;
        consumer.layerH = layer.height;
        consumer.registered = true;
        consumer.geometryDirty = true;
        compositor_.zc().BindSurface(consumer.surfaceKey, layer.shmCommitSerial);
        consumer.consecutiveFailures = 0;
        consumer.lastTimestamp = 0;
        consumer.timestampRegressions = 0;
        consumer.frames = 0;
        consumer.updates = 0;
        consumer.lastConsumedSignal = 0;
        consumer.coalescedSignals = 0;
        consumer.duplicateTimestamps = 0;
        consumer.failures = 0;
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] consumer attached tl=%{public}u key=%{public}llu "
                    "pid=%{public}u surface=%{public}u source=%{public}dx%{public}d "
                    "layer=%{public}dx%{public}d+%{public}d,%{public}d queue=%{public}d "
                    "size_ret=%{public}d usage_ret=%{public}d drop_ret=%{public}d",
                    rendererToplevelId,
                    static_cast<unsigned long long>(consumer.surfaceKey),
                    consumer.clientPid, consumer.surfaceId,
                    consumer.sourceW, consumer.sourceH, consumer.layerW, consumer.layerH,
                    consumer.layerX, consumer.layerY, queueSize,
                    sizeResult, usageResult, dropResult);
        consumer.fullscreen = layer.fullscreen && RootCompositing();
        consumer.layer = layer;
        zeroCopyConsumers_.push_back(std::move(state));
        zeroCopySceneDirty_ = true;
    }
    if (FrameTraceEnabled() && nowUs - zeroCopyDiagLastUs_ > 2000000) {
        zeroCopyDiagLastUs_ = nowUs;
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] multi-consumer tl=%{public}u attached=%{public}zu candidates=%{public}zu",
                    rendererToplevelId, zeroCopyConsumers_.size(), surfaces.size());
    }
    return !zeroCopyConsumers_.empty();
}

bool EglRenderer::UpdateZeroCopyFrame(ZeroCopyConsumer& consumer, int& width, int& height)
{
    if (!consumer.registered || !consumer.image ||
        !consumer.frameAvailable.exchange(false, std::memory_order_acq_rel))
        return false;

    const uint64_t signalCount = consumer.frameSignals.load(std::memory_order_acquire);
    const uint64_t signalDelta = signalCount >= consumer.lastConsumedSignal
        ? signalCount - consumer.lastConsumedSignal : 0;
    if (signalDelta > 1) consumer.coalescedSignals += signalDelta - 1;
    consumer.lastConsumedSignal = signalCount;
    ++consumer.updates;

    const int32_t updateResult = OH_NativeImage_UpdateSurfaceImage(consumer.image);
    const int32_t transformResult = updateResult == 0
        ? OH_NativeImage_GetTransformMatrixV2(consumer.image, consumer.transform) : -1;
    if (updateResult != 0 || transformResult != 0)
    {
        ++consumer.failures;
        ++consumer.consecutiveFailures;
        if (consumer.failures <= 10 || consumer.failures % 60 == 0)
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] update failed tl=%{public}u update=%{public}d "
                        "transform=%{public}d failures=%{public}llu consecutive=%{public}u",
                        toplevelId_.load(std::memory_order_relaxed), updateResult, transformResult,
                        static_cast<unsigned long long>(consumer.failures),
                        consumer.consecutiveFailures);
        if (compositor_.zc().IsReadyPublished(consumer.surfaceKey) &&
            !compositor_.zc().IsFallbackPending(consumer.surfaceKey) &&
            consumer.consecutiveFailures >= 8)
        {
            ZeroCopyLayerInfo layer;
            uint32_t rendererToplevelId = toplevelId_.load(std::memory_order_relaxed);
            if (RootCompositing())
                rendererToplevelId = compositor_.DesktopRootToplevelId();
            const bool baselineValid = compositor_.GetZeroCopyLayerInfo(
                consumer.surfaceKey, rendererToplevelId,
                consumer.sourceW, consumer.sourceH, layer);
            compositor_.zc().BeginFallback(consumer.surfaceKey, layer.shmCommitSerial,
                                           baselineValid, rendererToplevelId);
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] fallback pending tl=%{public}u key=%{public}llu "
                        "failures=%{public}u shm_baseline=%{public}llu",
                        rendererToplevelId,
                        static_cast<unsigned long long>(consumer.surfaceKey),
                        consumer.consecutiveFailures,
                        static_cast<unsigned long long>(
                            compositor_.zc().GetFallbackShmSerial(consumer.surfaceKey)));
        }
        // 2026-09-20 关键修复 (ROUND3 §7): 连续失败说明这一代 NativeImage/缓冲队列
        // 已不可用 (现场: 40601000 之后 producer 也不再交帧, app 这边因为
        // consumer.frameAvailable 已被取走而永远不再重试 → 双方互等)。
        // 重建消费者即为"归还队列缓冲 + 下轮重新 attach", 是打破该互等的唯一动作。
        if (consumer.consecutiveFailures >= 2)
        {
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] consumer re-attach after %{public}u consecutive "
                        "update failures tl=%{public}u key=%{public}llu failures=%{public}llu",
                        consumer.consecutiveFailures, toplevelId_.load(std::memory_order_relaxed),
                        static_cast<unsigned long long>(consumer.surfaceKey),
                        static_cast<unsigned long long>(consumer.failures));
            consumer.lastReattachUs = PerfNowUs();
            ++consumer.reattachCount;
            ReleaseZeroCopyBinding(consumer);
        }
        return false;
    }

    consumer.consecutiveFailures = 0;
    ComposeZeroCopySamplingTransform(consumer.transform, consumer.vulkanSource,
                                      consumer.samplingTransform);
    const int64_t imageTimestamp = OH_NativeImage_GetTimestamp(consumer.image);
    const int64_t previousTimestamp = consumer.lastTimestamp;
    int64_t timestampDeltaUs = 0;
    if (imageTimestamp > 0)
    {
        if (previousTimestamp > 0)
        {
            timestampDeltaUs = (imageTimestamp - previousTimestamp) / 1000;
            if (imageTimestamp < previousTimestamp) {
                ++consumer.timestampRegressions;
                if (consumer.timestampRegressions == 1 || consumer.timestampRegressions % 60 == 0)
                    OH_LOG_WARN(LOG_APP,
                                "[VIRGL-ZC][MAIN] timestamp regression tl=%{public}u "
                                "current=%{public}lld previous=%{public}lld count=%{public}llu",
                                toplevelId_.load(std::memory_order_relaxed), static_cast<long long>(imageTimestamp),
                                static_cast<long long>(previousTimestamp),
                                static_cast<unsigned long long>(consumer.timestampRegressions));
            } else if (imageTimestamp == previousTimestamp) {
                ++consumer.duplicateTimestamps;
            }
        }
        if (imageTimestamp > consumer.lastTimestamp)
            consumer.lastTimestamp = imageTimestamp;
    }

    ZeroCopyLayerInfo layer;
    uint32_t rendererToplevelId = toplevelId_.load(std::memory_order_relaxed);
    if (RootCompositing()) rendererToplevelId = compositor_.DesktopRootToplevelId();
    if (!compositor_.GetZeroCopyLayerInfo(consumer.surfaceKey, rendererToplevelId,
                                          consumer.sourceW, consumer.sourceH, layer) ||
        consumer.layer.bindingGeneration != layer.bindingGeneration)
    {
        ReleaseZeroCopyBinding(consumer);
        return false;
    }
    consumer.layerX = layer.x;
    consumer.layerY = layer.y;
    consumer.layerW = layer.width;
    consumer.layerH = layer.height;
    consumer.fullscreen = layer.fullscreen && RootCompositing();
    width = consumer.sourceW;
    height = consumer.sourceH;
    consumer.layer = layer;
    consumer.hasFrame = true;
    // 2026-09-20: 记录**真实**消费时刻 (供黑窗归因/活性判定; 旧实现在查询路径里刷)
    if (!compositor_.zc().NoteLayerConsumed(consumer.surfaceKey, PerfNowUs(), layer.bindingGeneration,
                                            consumer.sourceW, consumer.sourceH)) {
        ReleaseZeroCopyBinding(consumer);
        return false;
    }
    if (compositor_.zc().IsFallbackPending(consumer.surfaceKey))
    {
        compositor_.zc().CancelFallback(consumer.surfaceKey);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] fallback cancelled by GPU recovery tl=%{public}u key=%{public}llu",
                    rendererToplevelId,
                    static_cast<unsigned long long>(consumer.surfaceKey));
    }
    compositor_.zc().Activate(consumer.surfaceKey, rendererToplevelId);
    ++consumer.frames;
    if (FrameTraceEnabled() && consumer.frames <= 600)
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-ORDER][MAIN] frame=%{public}llu signals=%{public}llu "
                    "updates=%{public}llu signal_delta=%{public}llu coalesced=%{public}llu "
                    "timestamp=%{public}lld timestamp_delta_us=%{public}lld "
                    "timestamp_dup=%{public}llu timestamp_regress=%{public}llu",
                    static_cast<unsigned long long>(consumer.frames),
                    static_cast<unsigned long long>(consumer.frameSignals.load()),
                    static_cast<unsigned long long>(consumer.updates),
                    static_cast<unsigned long long>(signalDelta),
                    static_cast<unsigned long long>(consumer.coalescedSignals),
                    static_cast<long long>(imageTimestamp), static_cast<long long>(timestampDeltaUs),
                    static_cast<unsigned long long>(consumer.duplicateTimestamps),
                    static_cast<unsigned long long>(consumer.timestampRegressions));
    if (consumer.frames == 1 || consumer.frames % 120 == 0)
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] frame=%{public}llu tl=%{public}u key=%{public}llu "
                    "source=%{public}dx%{public}d layer=%{public}dx%{public}d+%{public}d,%{public}d "
                    "signals=%{public}llu failures=%{public}llu",
                    static_cast<unsigned long long>(consumer.frames), toplevelId_.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(consumer.surfaceKey), width, height,
                    consumer.layerW, consumer.layerH, consumer.layerX, consumer.layerY,
                    static_cast<unsigned long long>(consumer.frameSignals.load()),
                    static_cast<unsigned long long>(consumer.failures));
    return width > 0 && height > 0;
}

// 诊断 (2026-09-20, 默认关): WINEHUA_ZC_PIXEL_DUMP=<path>。在 ZC 层绘制之后立刻
// 读回该层在画布上的中心条带 → 回答"导入纹理本身是黑的"还是"合成后才黑"。
// 只在首帧/每 300 帧/自愈重连后 2s 内落盘, 避免常态读回开销。
void EglRenderer::DumpZeroCopyLayerPixels(ZeroCopyConsumer& consumer, int x, int y, int w, int h)
{
    // Read the host's startup setting once. Wine child Want env does not
    // enable host diagnostics; an unset path must keep GPU readback off.
    static const std::string dumpPath = [] {
        const char* path = std::getenv("WINEHUA_ZC_PIXEL_DUMP");
        return std::string(path ? path : "");
    }();
    if (dumpPath.empty() || consumer.dumpOpenFailed) return;

    if (!consumer.dumpFile)
    {
        consumer.dumpMax = 400;
        consumer.dumpFile = fopen(dumpPath.c_str(), "a");
        if (!consumer.dumpFile)
        {
            consumer.dumpOpenFailed = true;
            OH_LOG_WARN(LOG_APP, "[VIRGL-ZC][MAIN] pixel dump open failed path=%{public}s",
                        dumpPath.c_str());
            return;
        }
        fprintf(consumer.dumpFile,
                "# frame signals updates failures reattach nonblack sampled mean_luma\n");
    }
    if (w <= 0 || h <= 0) return;
    if (consumer.dumpCount >= consumer.dumpMax) return;
    const uint64_t nowUs = PerfNowUs();
    const bool afterReattach = consumer.lastReattachUs && nowUs - consumer.lastReattachUs < 2000000ull;
    if (!(consumer.frames <= 5 || consumer.frames % 300 == 0 || afterReattach)) return;

    const int rw = std::min(w, 256);
    const int rh = std::min(h, 64);
    const int rx = x + (w - rw) / 2;
    const int ry = y + (h - rh) / 2;
    std::vector<uint8_t> buf(static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4u);
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    size_t nonBlack = 0;
    uint64_t lumaSum = 0;
    const size_t pixels = buf.size() / 4;
    for (size_t i = 0; i + 3 < buf.size(); i += 4)
    {
        if (buf[i] | buf[i + 1] | buf[i + 2]) ++nonBlack;
        lumaSum += (buf[i] + buf[i + 1] + buf[i + 2]) / 3;
    }
    const uint64_t meanLuma = pixels ? lumaSum / pixels : 0;
    fprintf(consumer.dumpFile, "%llu %llu %llu %llu %llu %zu %zu %llu\n",
            static_cast<unsigned long long>(consumer.frames),
            static_cast<unsigned long long>(consumer.frameSignals.load()),
            static_cast<unsigned long long>(consumer.updates),
            static_cast<unsigned long long>(consumer.failures),
            static_cast<unsigned long long>(consumer.reattachCount),
            nonBlack, pixels, static_cast<unsigned long long>(meanLuma));
    fflush(consumer.dumpFile);
    if (consumer.dumpCount++ < 300)
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] pixel dump frame=%{public}llu tl=%{public}u "
                    "signals=%{public}llu updates=%{public}llu failures=%{public}llu "
                    "reattach=%{public}llu nonblack=%{public}zu/%{public}zu mean_luma=%{public}llu "
                    "rect=%{public}dx%{public}d+%{public}d,%{public}d",
                    static_cast<unsigned long long>(consumer.frames), toplevelId_.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(consumer.frameSignals.load()),
                    static_cast<unsigned long long>(consumer.updates),
                    static_cast<unsigned long long>(consumer.failures),
                    static_cast<unsigned long long>(consumer.reattachCount),
                    nonBlack, pixels, static_cast<unsigned long long>(meanLuma),
                    rw, rh, rx, ry);
}

void EglRenderer::ReleaseZeroCopyBinding(ZeroCopyConsumer& consumer)
{
    zeroCopySceneDirty_ = true;
    // Teardown logs below distinguish SurfaceQueue ownership failures from rendering failures.
    const uint64_t surfaceKey = consumer.surfaceKey;
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] release begin tl=%{public}u key=%{public}llu "
                "registered=%{public}d ready=%{public}d listener=%{public}d image=%{public}p",
                toplevelId_.load(std::memory_order_relaxed), static_cast<unsigned long long>(surfaceKey),
                consumer.registered, compositor_.zc().IsReadyPublished(surfaceKey),
                consumer.listenerSet, consumer.image);
    // 幂等: 未发布过 (attach 早退/从未 GPU_ACTIVE) 时状态复位序列是 no-op
    // (ready 未发布不撤也不打日志; key=0 时 SetEnabled 内部 no-op)
    compositor_.zc().Release(surfaceKey, toplevelId_.load(std::memory_order_relaxed));
    if (consumer.registered) {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release detach begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        winehua::GraphicsBroker::GetInstance().DetachZeroCopyTarget(consumer.surfaceKey);
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release detach end key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
    }
    consumer.registered = false;
    if (consumer.image && consumer.listenerSet) {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release listener-unset begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        const int32_t unsetResult = OH_NativeImage_UnsetOnFrameAvailableListener(consumer.image);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] release listener-unset end key=%{public}llu result=%{public}d",
                    static_cast<unsigned long long>(surfaceKey), unsetResult);
    }
    consumer.listenerSet = false;
    // The callback object outlives both listener removal and image destruction.
    consumer.producerWindow = nullptr;
    if (consumer.image) {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release image-destroy begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        OH_NativeImage_Destroy(&consumer.image);
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release image-destroy end key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
    }
    if (consumer.texture)
    {
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release texture-delete begin key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        glDeleteTextures(1, &consumer.texture);
        consumer.texture = 0;
        OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release texture-delete end key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
    }
    if (consumer.dumpFile) { fclose(consumer.dumpFile); consumer.dumpFile = nullptr; }
    consumer.frameAvailable.store(false, std::memory_order_release);
    consumer.hasFrame = false;
    consumer.geometryDirty = false;
    consumer.consecutiveFailures = 0;
    consumer.lastTimestamp = 0;
    consumer.timestampRegressions = 0;
    consumer.updates = 0;
    consumer.lastConsumedSignal = 0;
    consumer.coalescedSignals = 0;
    consumer.duplicateTimestamps = 0;
    consumer.clientPid = 0;
    consumer.surfaceId = 0;
    consumer.sourceW = 0;
    consumer.sourceH = 0;
    consumer.vulkanSource = false;
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] release complete tl=%{public}u key=%{public}llu",
                toplevelId_.load(std::memory_order_relaxed), static_cast<unsigned long long>(surfaceKey));
}

void EglRenderer::ShutdownZeroCopyConsumer()
{
    for (auto& consumer : zeroCopyConsumers_) ReleaseZeroCopyBinding(*consumer);
    zeroCopyConsumers_.clear();
    ClearZeroCopyShmTextures();
    zeroCopySnapshots_.clear();
    zeroCopyScene_ = {};
    if (zeroCopyProgram_) {
        glDeleteProgram(zeroCopyProgram_);
        zeroCopyProgram_ = 0;
    }
}

bool EglRenderer::SnapshotZeroCopyScene()
{
    if (!RootCompositing()) return false;
    std::vector<GpuDesktopLayer> sources;
    for (const auto& consumer : zeroCopyConsumers_) {
        if (!consumer->registered || !consumer->hasFrame) continue;
        const auto& info = consumer->layer;
        auto source = MakeNativeWindowLayer(consumer->surfaceKey, info,
                                            consumer->sourceW, consumer->sourceH);
        sources.push_back(std::move(source));
    }
    if (sources.empty()) {
        ClearZeroCopyShmTextures();
        zeroCopySnapshots_.clear();
        zeroCopyScene_ = {};
        return false;
    }
    return compositor_.SnapshotGpuDesktopScene({}, zeroCopySnapshots_, zeroCopyScene_, sources);
}

void EglRenderer::ClearZeroCopyShmTextures()
{
    for (auto& [key, saved] : zeroCopyShmTextures_) {
        static_cast<void>(key);
        if (saved.texture) glDeleteTextures(1, &saved.texture);
    }
    zeroCopyShmTextures_.clear();
}

void EglRenderer::DrawZeroCopyScene(winehua::TextureUploadStats* uploads)
{
    std::unordered_set<uint64_t> used;
    size_t drawn = 0;
    const bool diagnostic = zeroCopyScene_.diagnosticSerial &&
        (FrameTraceEnabled() || (winehua::FrameLoopDiagnosticState() & 1));
    const auto noteDraw = [&](const GpuDesktopLayer& layer) {
        if (diagnostic && drawn < 16)
            OH_LOG_INFO(LOG_APP, "[SCENE-DRAW-ISSUED] sample=%{public}llu order=%{public}zu "
                "at_us=%{public}llu top=%{public}u producer=%{public}llu owner=%{public}llu shm_serial=%{public}llu",
                static_cast<unsigned long long>(zeroCopyScene_.diagnosticSerial), drawn,
                static_cast<unsigned long long>(PerfNowUs()), layer.parentToplevel,
                static_cast<unsigned long long>(layer.zeroCopyKey),
                static_cast<unsigned long long>(layer.ownerSurfaceKey),
                static_cast<unsigned long long>(layer.serial));
        ++drawn;
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    // Wayland ARGB and native UI layers carry premultiplied alpha. Drawing the
    // original layers avoids copying a flattened black GPU hole over menus.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glScissor(letterbox_.offX, letterbox_.offY, letterbox_.dstW, letterbox_.dstH);
    for (const auto& layer : zeroCopyScene_.layers) {
        if (layer.w <= 0 || layer.h <= 0) continue;
        const int x = FitMapDisplayX(letterbox_, layer.x);
        const int y = FitMapDisplayY(letterbox_, frameH_ - layer.y - layer.h);
        const int w = std::max(1, FitSizeDisplayW(letterbox_, layer.w));
        const int h = std::max(1, FitSizeDisplayH(letterbox_, layer.h));
        glViewport(x, y, w, h);
        if (layer.solidBlack) {
            // glClear honors scissor, not viewport. Clear this backing at its
            // exact z-order, leaving later game content and popups untouched.
            const int left = std::max(x, letterbox_.offX);
            const int bottom = std::max(y, letterbox_.offY);
            const int right = std::min(x + w, letterbox_.offX + letterbox_.dstW);
            const int top = std::min(y + h, letterbox_.offY + letterbox_.dstH);
            if (right > left && top > bottom) {
                glDisable(GL_BLEND);
                glScissor(left, bottom, right - left, top - bottom);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                noteDraw(layer);
            }
            glScissor(letterbox_.offX, letterbox_.offY, letterbox_.dstW, letterbox_.dstH);
            continue;
        }
        if (layer.zeroCopyKey) {
            const auto found = std::find_if(zeroCopyConsumers_.begin(), zeroCopyConsumers_.end(),
                [&](const auto& consumer) { return consumer->surfaceKey == layer.zeroCopyKey; });
            if (found == zeroCopyConsumers_.end() || !(*found)->registered || !(*found)->hasFrame) continue;
            auto& consumer = **found;
            if (layer.opaque) glDisable(GL_BLEND); else glEnable(GL_BLEND);
            glUseProgram(zeroCopyProgram_);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, consumer.texture);
            glUniform1i(glGetUniformLocation(zeroCopyProgram_, "uTex"), 0);
            glUniform1f(glGetUniformLocation(zeroCopyProgram_, "uForceOpaque"), layer.opaque ? 1.f : 0.f);
            glUniformMatrix4fv(zeroCopyTransformLocation_, 1, GL_FALSE, consumer.samplingTransform);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            noteDraw(layer);
            continue;
        }
        if (!layer.pixels) continue;
        used.insert(layer.key);
        auto& saved = zeroCopyShmTextures_[layer.key];
        if (!saved.texture) {
            glGenTextures(1, &saved.texture);
            glBindTexture(GL_TEXTURE_2D, saved.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else glBindTexture(GL_TEXTURE_2D, saved.texture);
        const bool upload = saved.width != layer.sourceW || saved.height != layer.sourceH ||
                            saved.pixels != layer.pixels;
        const uint64_t uploadStart = upload && uploads && uploads->timeEnabled ? PerfNowUs() : 0;
        if (saved.width != layer.sourceW || saved.height != layer.sourceH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, layer.sourceW, layer.sourceH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, layer.pixels->data());
            saved.width = layer.sourceW; saved.height = layer.sourceH;
        } else if (saved.pixels != layer.pixels) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, layer.sourceW, layer.sourceH,
                            GL_RGBA, GL_UNSIGNED_BYTE, layer.pixels->data());
        }
        if (upload && uploads) uploads->Issued(layer.sourceW, layer.sourceH,
            uploads->timeEnabled ? PerfNowUs() - uploadStart : 0);
        saved.pixels = layer.pixels;
        if (layer.opaque) glDisable(GL_BLEND); else glEnable(GL_BLEND);
        glUseProgram(program_);
        glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
        glUniform1f(glGetUniformLocation(program_, "uForceOpaque"), layer.opaque ? 1.f : 0.f);
        glUniform3fv(glGetUniformLocation(program_, "uSampleU"), 1, layer.sampling.u.data());
        glUniform3fv(glGetUniformLocation(program_, "uSampleV"), 1, layer.sampling.v.data());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        noteDraw(layer);
    }
    if (diagnostic) OH_LOG_INFO(LOG_APP,
        "[SCENE-DRAW-END] sample=%{public}llu issued=%{public}zu truncated=%{public}zu",
        static_cast<unsigned long long>(zeroCopyScene_.diagnosticSerial), drawn, drawn > 16 ? drawn - 16 : 0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    for (auto it = zeroCopyShmTextures_.begin(); it != zeroCopyShmTextures_.end();) {
        if (used.count(it->first)) { ++it; continue; }
        glDeleteTextures(1, &it->second.texture);
        it = zeroCopyShmTextures_.erase(it);
    }
}

bool EglRenderer::Init(OHNativeWindow* window, int w, int h) {
    window_ = window;
    width_ = w;
    height_ = h;

    if (RootCompositing() && winehua::direct::DirectDesktopVulkanEnabled()) {
        vulkanDesktop_ = std::make_unique<winehua::direct::DirectVulkanDesktopCompositor>(compositor_);
        if (!vulkanDesktop_->Initialize(window)) { vulkanDesktop_.reset(); return false; }
        running_ = true;
        thread_ = std::thread(&EglRenderer::VulkanRenderLoop, this);
        return true;
    }

    // P0-GL-1: 首个窗口出现时做一次 Host EGL/GLES 能力探测 (后台线程, 单次)。
    // 放在这里是因为合成器一定会在会话早期走到, 且此时 EGL 已可用。
    WineHuaProbeHostGlCapability();

    OH_LOG_INFO(LOG_APP, "[EGL] Init tl=%{public}u req=%{public}dx%{public}d", toplevelId_.load(std::memory_order_relaxed), w, h);

    // 1. 使用共享 EGLDisplay (全进程只 init 一次)
    display_ = GetSharedDisplay();
    if (display_ == EGL_NO_DISPLAY) {
        OH_LOG_ERROR(LOG_APP, "[EGL] shared display unavailable tl=%{public}u", toplevelId_.load(std::memory_order_relaxed));
        return false;
    }

    EGLConfig cfg = nullptr;
    EGLint nCfg = 0;
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    if (!eglChooseConfig(display_, attrs, &cfg, 1, &nCfg) || nCfg < 1) {
        OH_LOG_ERROR(LOG_APP, "[EGL] no usable window config error=0x%{public}x", eglGetError());
        return false;
    }

    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (context_ == EGL_NO_CONTEXT) {
        OH_LOG_ERROR(LOG_APP, "[EGL] context creation failed error=0x%{public}x", eglGetError());
        return false;
    }

    // 异型窗口 (layered/shaped): 确保 native window buffer 带 alpha 通道,
    // 否则 per-pixel alpha 在 buffer 层就被丢弃 (默认可能是 RGBX)
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);

    // OHOS: EGLNativeWindowType = OHNativeWindow* (cast to unsigned long)
    surface_ = eglCreateWindowSurface(display_, cfg,
                                       reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed tl=%{public}u: 0x%{public}x", toplevelId_.load(std::memory_order_relaxed), eglGetError());
        return false;
    }
    {
        EGLint sw = 0, sh = 0, alphaBits = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &sw);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &sh);
        eglQuerySurface(display_, surface_, EGL_ALPHA_SIZE, &alphaBits);
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u eglSurface %{public}dx%{public}d alphaBits=%{public}d",
                    toplevelId_.load(std::memory_order_relaxed), sw, sh, alphaBits);
    }

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Init done, render thread started OK", toplevelId_.load(std::memory_order_relaxed));
    return true;
}

FitRect EglRenderer::GetInputLetterbox() const {
    // 输入逆映射锚 (PresentedFrame 契约, 重构第 2B 步): 用最近一帧契约的
    // 逻辑内容尺寸 (contentW/H) 对当前 surface 保比例 fit。桌面合成/快进/
    // 直传帧均锚桌面逻辑尺寸, PC 窗口帧锚窗口内容尺寸。与显示 letterbox
    // (锚 buffer 尺寸 frame.w/h) 解耦: 直传游戏帧 buffer 是内容尺寸 (如
    // 800x600), 输入锚仍是桌面逻辑尺寸 (如 1400x920) — 否则逆映射二次缩放
    // (红警2 主菜单点击无效根因)。锚未就绪 (首帧前 contentW/H=0) 或 fit 失败
    // 退回显示 letterbox (与旧 CoordTransform fallback 语义一致)。
    if (contentW_ > 0 && contentH_ > 0) {
        FitRect lb;
        if (ComputeFitRect(width_, height_, contentW_, contentH_, lb)) return lb;
    }
    return letterbox_;
}

FitRect EglRenderer::ComputeFrameDisplayRect(int drawW, int drawH) const {
    // 常态: 与等比映射锚同值 (整帧按比例显示, 多余部分是黑边)
    if (!stretchFill_.load() || drawW <= 0 || drawH <= 0) {
        return letterbox_;
    }
    // 拖拽缩放中: 填满 surface — srcW/srcH/scale 沿用映射锚, 只覆盖显示目标
    // (拖拽的过渡帧不保持比例, Wine 新帧到达后由拖拽结束的 configure 复位)
    FitRect r = letterbox_;
    r.offX = 0;
    r.offY = 0;
    r.dstW = drawW;
    r.dstH = drawH;
    return r;
}

uint32_t EglRenderer::DirectPassCapabilities() const
{
    // 直传能力位 (任务 3, 行为平价): 渲染器 GL 行为是 SHM 全屏直传逐像素
    // 等价的前提, 此前散在 compositor 侧注释假设 — 能力来源逐条对应:
    // - kForceOpaqueNoBlend: 本 context 从不开启 GL_BLEND (RenderFrame 注释)
    //   + uForceOpaque 按 frameArgb_ 强制不透明 (egl_renderer.cpp:874);
    // - kFitSameAsCpu: 几何统一由 ComputeFitRect 计算, 与 CPU 合成/输入命中
    //   同源 (egl_renderer.cpp:819-821);
    // - kXrgbFrameOpaque: root XRGB → frameArgb_=false, GPU 黑边不透明
    //   (直传帧整屏覆盖有效)。
    // 当前实现恒备全部能力 → 合成侧查询恒通过 (无能力位时判定不变)。
    return winehua::kDirectPassCapabilitiesAll;
}

void EglRenderer::VulkanRenderLoop() {
    while (running_) {
        const auto request = repaint_.CaptureRequest();
        const auto serial = vulkanDesktop_->PresentationSerial();
        if (!vulkanDesktop_->Render(expectW_ > 0 ? expectW_ : width_, expectH_ > 0 ? expectH_ : height_,
                renderPaused_.load(std::memory_order_acquire), repaint_.NeedsRedraw(request))) break;
        if (vulkanDesktop_->PresentationSerial() != serial) repaint_.DidPresent(request);
        if (renderPaused_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(vsyncMutex_);
            vsyncCv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return !running_ || !renderPaused_.load(std::memory_order_acquire);
            });
            continue;
        }
        width_ = vulkanDesktop_->Width(); height_ = vulkanDesktop_->Height();
        frameW_ = contentW_ = vulkanDesktop_->ContentWidth();
        frameH_ = contentH_ = vulkanDesktop_->ContentHeight();
        ComputeFitRect(width_, height_, frameW_, frameH_, letterbox_);
        // FIFO present paces active frames. Before the first root exists,
        // yield instead of spinning while the Wine desktop initializes.
        if (frameW_ <= 0 || frameH_ <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    vulkanDesktop_.reset(); // drain all GPU reads before XComponent destruction
    running_ = false;
}

void EglRenderer::RenderLoop() {
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglMakeCurrent failed: 0x%{public}x", eglGetError());
        running_ = false;
        return;
    }

    // NativeVSync owns frame scheduling. Disable EGL's independent swap pacing
    // so a frame does not wait once for VSync and again inside eglSwapBuffers.
    const bool swapIntervalDisabled = eglSwapInterval(display_, 0) == EGL_TRUE;
    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u eglSwapInterval(0)=%{public}s",
                toplevelId_.load(std::memory_order_relaxed), swapIntervalDisabled ? "OK" : "FAIL");

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

    // 4. 纹理 (初始空)
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const bool zeroCopyReady = InitZeroCopyConsumer();
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] tl=%{public}u path=%{public}s",
                toplevelId_.load(std::memory_order_relaxed), zeroCopyReady ? "SURFACE_QUEUE" : "SHM_FALLBACK");

    // 5. 渲染循环: 跟随硬件 VSync, 每次只取最新的 toplevel 帧
    FpsCounter fps("render");
    std::vector<uint8_t> px;
    int fw = 0, fh = 0;
    int loopCount = 0;
    bool firstFrameLogged = false;
    bool rendered = false;  // 首帧已渲染后, 无新帧时跳过 GPU 绘制
    RendererPerfWindow perf;
    winehua::FrameLoopDiagnosticWindow loopPerf;
    uint32_t diagnosticRoot = toplevelId_.load(std::memory_order_relaxed);
    GpuDesktopScene previousZeroCopyScene;
    uint32_t pixSampleN = 0;  // [PIX-SAMPLE] 帧内容白度采样计数 (诊断)

    static constexpr long long kFallbackPeriodNs = 16666667;
    static constexpr auto kVSyncTimeout = std::chrono::milliseconds(100);
    const char vsyncName[] = "WineHuaRenderer";
    OH_NativeVSync* nativeVsync = OH_NativeVSync_Create(vsyncName, sizeof(vsyncName) - 1);
    if (nativeVsync) {
        OH_NativeVSync_ExpectedRateRange expectedRate = {60, 120, 120};
        const int rateResult = winehua::RequestDesktopRefreshRange(nativeVsync);
        OH_LOG_INFO(LOG_APP,
                    "[MW-RNDR] tl=%{public}u request frame rate min=%{public}d "
                    "max=%{public}d expected=%{public}d result=%{public}d",
                    toplevelId_.load(std::memory_order_relaxed), expectedRate.min, expectedRate.max,
                    expectedRate.expected, rateResult);
    }
    long long vsyncPeriodNs = vsyncPeriodNs_.load(std::memory_order_relaxed);
    long long loggedPeriodNs = 0;
    unsigned int vsyncFailures = 0;
    auto fallbackDeadline = PerfClock::now();

    auto waitForFrameTickImpl = [&]() -> bool {
        if (!running_) return false;

        if (nativeVsync) {
            if (loopPerf.Active()) ++loopPerf.vsyncRequests;
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
                            for (const auto& consumer : zeroCopyConsumers_)
                                winehua::GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(
                                    consumer->surfaceKey, static_cast<uint64_t>(period));
                        }
                        const long long periodDelta = period > loggedPeriodNs
                            ? period - loggedPeriodNs : loggedPeriodNs - period;
                        if (loggedPeriodNs == 0 || periodDelta >= 500000) {
                            const double refreshRate = 1000000000.0 / static_cast<double>(period);
                            OH_LOG_INFO(LOG_APP,
                                        "[MW-RNDR] tl=%{public}u NativeVSync period=%{public}lldns "
                                        "rate=%{public}.2fHz",
                                        toplevelId_.load(std::memory_order_relaxed), period, refreshRate);
                            loggedPeriodNs = period;
                        }
                    }
                    vsyncFailures = 0;
                    fallbackDeadline = PerfClock::now();
                    return true;
                }
                if (loopPerf.Active()) ++loopPerf.vsyncTimeouts;
            } else {
                if (loopPerf.Active()) ++loopPerf.vsyncErrors;
            }

            ++vsyncFailures;
            if (vsyncFailures == 1 || vsyncFailures % 120 == 0) {
                OH_LOG_WARN(LOG_APP,
                            "[MW-RNDR] tl=%{public}u NativeVSync unavailable "
                            "request=%{public}d failures=%{public}u, using deadline fallback",
                            toplevelId_.load(std::memory_order_relaxed), requestResult, vsyncFailures);
            }
        }

        const auto period = std::chrono::nanoseconds(
            vsyncPeriodNs > 0 ? vsyncPeriodNs : kFallbackPeriodNs);
        const auto now = PerfClock::now();
        fallbackDeadline += period;
        if (fallbackDeadline <= now || fallbackDeadline - now > period * 2)
            fallbackDeadline = now + period;
        if (loopPerf.Active()) ++loopPerf.fallbacks;
        std::this_thread::sleep_until(fallbackDeadline);
        return running_;
    };
    auto waitForFrameTick = [&]() -> bool {
        const uint64_t startedUs = loopPerf.Active() ? PerfNowUs() : 0;
        const bool result = waitForFrameTickImpl();
        if (loopPerf.Active()) {
            const uint64_t nowUs = PerfNowUs();
            loopPerf.NoteWait(nowUs - startedUs);
            loopPerf.MaybePublish(toplevelId_.load(std::memory_order_relaxed), diagnosticRoot, nowUs);
        }
        return result;
    };

    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u render loop started pacing=%{public}s",
                toplevelId_.load(std::memory_order_relaxed), nativeVsync ? "NativeVSync" : "deadline-60Hz");

    std::unique_ptr<winehua::direct::DirectDesktopCompositor> directDesktop;
    uint32_t directScope = 0;

    bool wasPaused = false;
    uint32_t lastIdentity = GetToplevelId();
    while (running_) {
        const uint64_t repaintRequest = repaint_.CaptureRequest();
        const uint32_t identity = GetToplevelId();
        if (identity != lastIdentity) {
            rendered = false;
            lastDrawW_ = lastDrawH_ = 0;
            previousZeroCopyScene = {};
            compositor_.RequestFullRedraw(identity);
            lastIdentity = identity;
        }
        // External Linux carriers own Wayland buffers, not OHOS Wine Direct
        // queues. Ownership can arrive after renderer creation: retire its
        // old imports before the Wine desktop attempts to claim those queues,
        // including while this carrier is paused in the background.
        const bool directEnabled = !isolatedToplevel_.load(std::memory_order_acquire);
        const uint32_t requestedScope = RootCompositing() ? 0 : identity;
        if (directDesktop && (!directEnabled || directScope != requestedScope)) directDesktop.reset();
        if (directEnabled && !directDesktop) {
            directDesktop = std::make_unique<winehua::direct::DirectDesktopCompositor>(
                compositor_, display_, requestedScope);
            directScope = requestedScope;
        }
        const bool paused = renderPaused_.load(std::memory_order_acquire);
        if (paused != wasPaused) {
            const uint64_t period = paused ? 100000000ull :
                static_cast<uint64_t>(vsyncPeriodNs > 0 ? vsyncPeriodNs : kFallbackPeriodNs);
            for (const auto& consumer : zeroCopyConsumers_)
                winehua::GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(consumer->surfaceKey, period);
            if (!paused) {
                rendered = false;
                lastDrawW_ = lastDrawH_ = 0;
                previousZeroCopyScene = {};
                compositor_.RequestFullRedraw(
                    RootCompositing() ? compositor_.DesktopRootToplevelId() : GetToplevelId());
            }
            wasPaused = paused;
        }
        const uint64_t frameStartedUs = PerfNowUs();
        const bool loopDiagnostics = loopPerf.Sync(winehua::FrameLoopDiagnosticState(), frameStartedUs);
        winehua::ScopedTakeLockSink lockSink(loopDiagnostics ? &loopPerf.lockWait : nullptr);
        const uint64_t takeStartedUs = frameStartedUs;
        winehua::TextureUploadStats uploads;
        uploads.timeEnabled = loopDiagnostics || FrameTraceEnabled();
        bool haveFrame = false;
        bool cpuFrame = false;
        bool zeroCopyFrame = false;
        int zeroCopyWidth = 0;
        int zeroCopyHeight = 0;
        uint32_t useToplevel = toplevelId_.load(std::memory_order_relaxed);
        // Desktop mode: root toplevel may be recreated, always use current ID
        // (6A: 配置经构造注入的 DesktopCompositor 引用直读 — 与旧
        // WaylandServer::Policy()/GetDesktopRootToplevelId() 同一引用成员, 同值)
        if (RootCompositing()) useToplevel = compositor_.DesktopRootToplevelId();
        diagnosticRoot = useToplevel;
        const bool directFrame = directDesktop && directDesktop->Update();
        TryAttachZeroCopySurface(useToplevel);
        bool zeroCopyGeometryFrame = zeroCopySceneDirty_;
        zeroCopySceneDirty_ = false;
        for (auto& state : zeroCopyConsumers_) {
            auto& consumer = *state;
            zeroCopyGeometryFrame |= consumer.geometryDirty;
            consumer.geometryDirty = false;
            zeroCopyFrame |= UpdateZeroCopyFrame(consumer, zeroCopyWidth, zeroCopyHeight);
            // 2026-09-20 自愈 (ROUND3 §7): 已 attach 但真实 present 长时间不来, 且当前
            // 消费者侧"没有可用内容"(从未消费成功)或"已经出现过失败" → 这一代
            // NativeImage/队列很可能已被 producer 弃用, 重建消费者归还缓冲,
            // 下一轮 TryAttachZeroCopySurface 重新 attach。保守条件是为了不打扰
            // 正常的静态窗口 (静止但健康的窗口会一直 frames>0/failures==0)。
            if (consumer.registered && consumer.surfaceKey)
            {
                const uint64_t idleNowUs = PerfNowUs();
                const uint64_t lastSignalUs = consumer.lastSignalUs.load(std::memory_order_relaxed);
                const uint64_t baseUs = lastSignalUs ? lastSignalUs : consumer.attachUs;
                const uint64_t idleMs = baseUs && idleNowUs > baseUs ? (idleNowUs - baseUs) / 1000 : 0;
                const bool neverConsumed = (consumer.frames == 0 && idleMs > 5000);
                const bool failedBefore = (consumer.failures > 0 && idleMs > 8000);
                if ((neverConsumed || failedBefore) &&
                    idleNowUs - consumer.lastReattachUs > 3000000ull)
                {
                    consumer.lastReattachUs = idleNowUs;
                    ++consumer.reattachCount;
                    OH_LOG_WARN(LOG_APP,
                                "[VIRGL-ZC][MAIN] stale consumer re-attach tl=%{public}u "
                                "key=%{public}llu idleMs=%{public}llu signals=%{public}llu "
                                "frames=%{public}llu failures=%{public}llu reattach=%{public}llu "
                                "reason=%{public}s",
                                toplevelId_.load(std::memory_order_relaxed),
                                static_cast<unsigned long long>(consumer.surfaceKey),
                                static_cast<unsigned long long>(idleMs),
                                static_cast<unsigned long long>(consumer.frameSignals.load()),
                                static_cast<unsigned long long>(consumer.frames),
                                static_cast<unsigned long long>(consumer.failures),
                                static_cast<unsigned long long>(consumer.reattachCount),
                                neverConsumed ? "never-consumed" : "update-failed");
                    ReleaseZeroCopyBinding(consumer);
                }
            }
        }
        if (paused) {
            std::unique_lock<std::mutex> lock(vsyncMutex_);
            vsyncCv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return !running_ || !renderPaused_.load(std::memory_order_acquire);
            });
            continue;
        }
        const bool zeroCopySceneReady = SnapshotZeroCopyScene();
        if (zeroCopySceneReady) {
            cpuFrame = !SameGpuDesktopScene(zeroCopyScene_, previousZeroCopyScene);
            previousZeroCopyScene = zeroCopyScene_;
            fw = frameW_ = contentW_ = zeroCopyScene_.width;
            fh = frameH_ = contentH_ = zeroCopyScene_.height;
            frameArgb_ = false;
        } else previousZeroCopyScene = {};
        if (!zeroCopySceneReady && useToplevel != 0) {
            // 帧交付契约 (presented_frame.h): TakeToplevelFrame 返回 PresentedFrame。
            // fw/fh 从帧 buffer 尺寸 (frame.w/h) 取 — 直传帧是游戏内容尺寸
            // (如 800x600), 合成/快进帧是桌面逻辑尺寸 (如 1400x920); 显示
            // letterbox 用它们 (frameW_/frameH_)。alpha 语义取 frame.opaque —
            // 产出侧 opaque = ShmFormat != 0, 故 !opaque == 旧 (ShmFormat==0) 等价。
            // contentW/H (逻辑内容尺寸) 缓存供 GetInputLetterbox 输入逆映射锚。
            PresentedFrame frame;
            cpuFrame = compositor_.TakeToplevelFrame(useToplevel, px, frame);
            if (cpuFrame) {
                fw = frame.w;
                fh = frame.h;
                frameArgb_ = !frame.opaque;
                contentW_ = frame.contentW;
                contentH_ = frame.contentH;
            }
        }
        haveFrame = cpuFrame || zeroCopyFrame || zeroCopyGeometryFrame || directFrame;
        const uint64_t takeUs = PerfNowUs() - takeStartedUs;
        loopPerf.NoteTake(takeUs, haveFrame, cpuFrame, zeroCopyFrame, directFrame, zeroCopyGeometryFrame);

        if (!zeroCopySceneReady && cpuFrame && fw > 0 && fh > 0) {
            // 存储帧尺寸供输入坐标转换
            frameW_ = fw;
            frameH_ = fh;
            // 帧级诊断 (默认关闭, WINEHUA_FRAME_TRACE=1 开启): CPU 帧尺寸/序号
            if (FrameTraceEnabled()) {
                OH_LOG_INFO(LOG_APP, "[DBG-CPU] tl=%{public}u fw=%{public}d fh=%{public}d px=%{public}zu firstLogged=%{public}d loop=%{public}d",
                            useToplevel, fw, fh, px.size(), firstFrameLogged, loopCount);
            }
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
            const uint64_t uploadStartedUs = uploads.timeEnabled ? PerfNowUs() : 0;
            if (fw != texW_ || fh != texH_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                texW_ = fw; texH_ = fh;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh,
                                GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            }
            uploads.Issued(fw, fh, uploads.timeEnabled ? PerfNowUs() - uploadStartedUs : 0);
            if (rowLen != fw) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            rendered = true;
            // [PIX-SAMPLE] 帧内容白度采样: 区分"帧本身是白的" (guest/wine
            // 侧渲染或回读) vs "帧有内容但显示白" (宿主 EGL/WMS 侧)。
            // 每 30 帧对 px 采样 (中心像素 RGB + 全宽白像素占比)。
            if (++pixSampleN % 30 == 1) {
                size_t whitePixels = 0, totalPx = px.size() / 4;
                // 采样: 每 16 像素取 1 个像素
                if (totalPx > 0) {
                    for (size_t i = 0; i < totalPx; i += 16) {
                        const uint8_t* p = &px[i * 4];
                        if (p[0] == 255 && p[1] == 255 && p[2] == 255) ++whitePixels;
                    }
                    const uint8_t* mid = &px[(totalPx / 2) * 4];
                    OH_LOG_INFO(LOG_APP,
                                "[PIX-SAMPLE] tl=%{public}u fw=%{public}d fh=%{public}d "
                                "whitePx=%{public}zu/%{public}zu midRGB=(%{public}u,%{public}u,%{public}u)",
                                useToplevel, fw, fh, whitePixels, totalPx / 16 + 1,
                                mid[0], mid[1], mid[2]);
                }
            }
        }
        if (zeroCopyFrame && !firstFrameLogged) {
            OH_LOG_INFO(LOG_APP,
                        "[MW-RNDR] tl=%{public}u FIRST ZERO-COPY FRAME %{public}dx%{public}d",
                        useToplevel, zeroCopyWidth, zeroCopyHeight);
            firstFrameLogged = true;
        }

        // 无新帧且已渲染过首帧 → 跳过 GPU 绘制, 静态桌面节省 GPU 功耗。
        // 例外: EGL surface 与"上次真正画上去的尺寸"不一致 (最小化还原/窗口
        // resize/沉浸式切换的中间态落地延迟) 必须重新 letterbox 上屏 — 否则
        // 最后一帧留在旧尺寸 buffer 上, 静止窗口再无新帧触发重绘, 系统把旧
        // buffer 拉伸显示导致缩放错误 (2026-09-14 全屏桌面左右黑边根因)。
        // 判据必须是 lastDrawW_/lastDrawH_ (= 上次实际上屏的矩形): 用
        // width_/height_ 会被 SetSize 写成 ArkTS 的"声明尺寸"而误判 — 系统把
        // buffer 切到 1840 后, "实测 1840 == 声明的 1840" 成立就永不重绘了。
        if (!haveFrame && (rendered || zeroCopySceneReady)) {
            EGLint curW = 0, curH = 0;
            eglQuerySurface(display_, surface_, EGL_WIDTH, &curW);
            eglQuerySurface(display_, surface_, EGL_HEIGHT, &curH);
            if (curW == lastDrawW_ && curH == lastDrawH_ && !repaint_.NeedsRedraw(repaintRequest)) {
                loopCount++;
                ++skipFrames_;   // 诊断: 统计跳过 swap 的帧数
                if (loopDiagnostics) {
                    ++loopPerf.skipped;
                    loopPerf.NoteWork(PerfNowUs() - frameStartedUs);
                }
                if (!waitForFrameTick()) break;
                continue;
            }
            OH_LOG_INFO(LOG_APP,
                        "[MW-RNDR] tl=%{public}u surface %{public}dx%{public}d -> %{public}dx%{public}d with no new frame, re-letterbox",
                        useToplevel, lastDrawW_, lastDrawH_, curW, curH);
        }

        // 获取 EGL surface 实际大小 (本次绘制的目标 buffer 尺寸)
        EGLint surfW = 0, surfH = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &surfW);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfH);
        // [MW-RNDR] 声明尺寸 (ArkTS SetSize) 与实际 surface 不一致时告警 — 系统侧
        // 没采纳声明的信号; 此时画面按实际尺寸 letterbox (几何正确), 但若它长期
        // 不收敛, 说明上游声明值本身可疑。限频: 数值变化才打, 否则未落地稳态每帧刷。
        if (surfW > 0 && surfH > 0 && expectW_ > 0 && expectH_ > 0 &&
            (surfW != expectW_ || surfH != expectH_) &&
            (surfW != lastWarnSurfW_ || surfH != lastWarnSurfH_)) {
            lastWarnSurfW_ = surfW; lastWarnSurfH_ = surfH;
            OH_LOG_WARN(LOG_APP,
                        "[MW-RNDR] tl=%{public}u DRAW-TIME surface=%{public}dx%{public}d != expect=%{public}dx%{public}d",
                        toplevelId_.load(std::memory_order_relaxed), surfW, surfH, expectW_, expectH_);
        }
        if (surfW > 0 && surfH > 0) {
            width_ = surfW;
            height_ = surfH;
        }
        // 本次绘制的尺寸快照: 绘制期间 width_/height_ 可能被 NAPI 线程的 SetSize
        // 改写 (日志实证: swap 后 w=2800 h=1840 而 lb 仍是按 1683 算的), 所以
        // letterbox / viewport / 上屏记录三者必须共用这一份快照, 保证"画的"
        // 与"记的"是同一个值
        const int drawW = width_, drawH = height_;

        // 等比映射锚: 帧坐标空间 → surface 的保比例 fit。几何统一由
        // ComputeFitRect 计算 (与 desktop 合成/输入命中同源; 历史实现此处
        // 独立手写, 截断取整与合成的 lround 不一致曾有 1px 偏差)。
        // 消费方: ZC 层映射/遮挡重绘/输入逆映射, 以及常态下的整帧显示 —
        // 拖拽缩放中的整帧显示矩形另见 ComputeFrameDisplayRect。
        if (!ComputeFitRect(drawW, drawH, frameW_, frameH_, letterbox_)) {
            letterbox_ = FitRect{};
        }
        // [DBG-FIT] 几何变化时打印一条 (surface/frame/letterbox 任一变化)。
        // 采样式 %60 对"绘制次数"取模会吞掉关键那次绘制, 故改为变化即打;
        // 记录值必须是本实例成员 — 每个 toplevel 一个渲染器, 函数内 static 会被
        // 多个渲染线程互相覆盖 (2026-09-14 审查发现)。
        if (drawW != lastFitLogW_ || drawH != lastFitLogH_ ||
            frameW_ != lastFitLogFw_ || frameH_ != lastFitLogFh_ ||
            letterbox_.dstW != lastFitLogLbW_ || letterbox_.dstH != lastFitLogLbH_ ||
            drawW == 0 || drawH == 0) {
            lastFitLogW_ = drawW; lastFitLogH_ = drawH;
            lastFitLogFw_ = frameW_; lastFitLogFh_ = frameH_;
            lastFitLogLbW_ = letterbox_.dstW; lastFitLogLbH_ = letterbox_.dstH;
            OH_LOG_INFO(LOG_APP, "[DBG-FIT] tl=%{public}u surface=%{public}dx%{public}d frame=%{public}dx%{public}d lb=%{public}dx%{public}d+%{public}d,%{public}d zc=%{public}d/%{public}d/%{public}d",
                        useToplevel, drawW, drawH, frameW_, frameH_,
                        letterbox_.dstW, letterbox_.dstH, letterbox_.offX, letterbox_.offY,
                        !zeroCopyConsumers_.empty() ? 1 : 0, zeroCopySceneReady ? 1 : 0, 0);
        }

        // 诊断: 前10帧详细打印 surface -> frame -> viewport 完整映射
        if (loopCount < 10) {
            int barTop = letterbox_.offY;
            int barBot = drawH - letterbox_.offY - letterbox_.dstH;
            int barLeft = letterbox_.offX;
            int barRight = drawW - letterbox_.offX - letterbox_.dstW;
            float sA = (float)drawW / drawH;
            float fA = frameW_ > 0 && frameH_ > 0 ? (float)frameW_ / frameH_ : 0;
            OH_LOG_INFO(LOG_APP, "[MW-RNDR] diag#%{public}d tl=%{public}u surface=%{public}dx%{public}d(asp=%{public}.2f) frame=%{public}dx%{public}d(asp=%{public}.2f) vp=%{public}dx%{public}d+%{public}d,%{public}d bar=(L%{public}d R%{public}d T%{public}d B%{public}d)",
                        loopCount, useToplevel,
                        drawW, drawH, sA, frameW_, frameH_, fA,
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

        if (rendered && !zeroCopySceneReady) {
            const FitRect disp = ComputeFrameDisplayRect(drawW, drawH);
            glViewport(disp.offX, disp.offY, disp.dstW, disp.dstH);
            glUseProgram(program_);
            glUniform3f(glGetUniformLocation(program_, "uSampleU"), 0.f, 1.f, 0.f);
            glUniform3f(glGetUniformLocation(program_, "uSampleV"), 0.f, 0.f, 1.f);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
            glUniform1f(glGetUniformLocation(program_, "uForceOpaque"), frameArgb_ ? 0.0f : 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        if (zeroCopySceneReady) DrawZeroCopyScene(&uploads);
        else for (auto& state : zeroCopyConsumers_) {
            auto& consumer = *state;
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        if (consumer.hasFrame && consumer.registered && frameW_ > 0 && frameH_ > 0 &&
            consumer.layerW > 0 && consumer.layerH > 0) {
            if ((consumer.frames == 1 || consumer.frames % 120 == 0))
                OH_LOG_INFO(LOG_APP, "[DBG-ZC] tl=%{public}u lb=%{public}dx%{public}d+%{public}d,%{public}d frame=%{public}dx%{public}d layer=%{public}dx%{public}d+%{public}d,%{public}d fs=%{public}d src=%{public}dx%{public}d",
                            useToplevel, letterbox_.dstW, letterbox_.dstH, letterbox_.offX, letterbox_.offY,
                            frameW_, frameH_, consumer.layerW, consumer.layerH,
                            consumer.layerX, consumer.layerY, consumer.fullscreen,
                            consumer.sourceW, consumer.sourceH);
            int layerViewportX, layerViewportY, layerViewportW, layerViewportH;
            if (consumer.fullscreen) {
                // ZC 游戏全屏: 层内容保比例缩放进桌面帧的显示区, 而非按帧比例
                // 映射 — 全屏后 buffer 被 Wine 扩到输出尺寸, 但层几何仍是游戏
                // 内部分辨率, 直接映射会把画面缩到左上角一块。CPU 侧整帧已填黑
                // (TakeToplevelFrame ZC 分支), 这里的 letterbox 与 SHM 全屏同效
                FitRect zcFit;
                if (ComputeFitRect(letterbox_.dstW, letterbox_.dstH,
                                   consumer.layerW, consumer.layerH, zcFit)) {
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
                layerViewportX = FitMapDisplayX(letterbox_, consumer.layerX);
                layerViewportY = FitMapDisplayY(letterbox_, frameH_ - consumer.layerY - consumer.layerH);
                layerViewportW = std::max(1, FitSizeDisplayW(letterbox_, consumer.layerW));
                layerViewportH = std::max(1, FitSizeDisplayH(letterbox_, consumer.layerH));
            }
            glViewport(layerViewportX, layerViewportY, layerViewportW, layerViewportH);
            glUseProgram(zeroCopyProgram_);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, consumer.texture);
            glUniform1i(glGetUniformLocation(zeroCopyProgram_, "uTex"), 0);
            glUniform1f(glGetUniformLocation(zeroCopyProgram_, "uForceOpaque"), consumer.vulkanSource ? 1.f : 0.f);
            glUniformMatrix4fv(zeroCopyTransformLocation_, 1, GL_FALSE,
                               consumer.samplingTransform);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            DumpZeroCopyLayerPixels(consumer, layerViewportX, layerViewportY,
                                    layerViewportW, layerViewportH);

        }
        }

        if (directDesktop) directDesktop->Draw(useToplevel, frameW_, frameH_, letterbox_);

        const uint64_t swapStartedUs = PerfNowUs();
        if (renderPaused_.load(std::memory_order_acquire)) continue;
        const bool swapOk = eglSwapBuffers(display_, surface_) == EGL_TRUE;
        const EGLint swapError = swapOk ? EGL_SUCCESS : eglGetError();
        // 记录"这次真正上屏的绘制尺寸" — 无帧循环据此判断当前 surface 是否已经
        // 与画面不一致 (不一致就重绘)。swap 失败时不记录, 下一轮会再试。
        if (swapOk) {
            repaint_.DidPresent(repaintRequest);
            gAcceptedPresents.fetch_add(1, std::memory_order_relaxed);
            if (zeroCopyFrame) gAcceptedGpuPresents.fetch_add(1, std::memory_order_relaxed);
            lastDrawW_ = drawW;
            lastDrawH_ = drawH;
            swapFailStreak_ = 0;
        } else {
            repaint_.RequestRedraw();
            ++swapFailStreak_;
            if (swapFailStreak_ == 1 || swapFailStreak_ % 120 == 0) {
            // swap 失败: 画面没上屏, 循环会持续重试绘制 — 首次 + 每 120 次低频
            // 告警, 便于发现 surface 失效/buffer 饥饿这类持续失败
            OH_LOG_WARN(LOG_APP,
                        "[MW-RNDR] tl=%{public}u eglSwapBuffers failed x%{public}d error=0x%{public}x (draw=%{public}dx%{public}d)",
                        toplevelId_.load(std::memory_order_relaxed), swapFailStreak_, swapError, drawW, drawH);
            }
            if (swapFailStreak_ >= 3 && (swapError == EGL_BAD_SURFACE ||
                swapError == EGL_BAD_NATIVE_WINDOW || swapError == EGL_CONTEXT_LOST)) {
                running_ = false;
                break;
            }
        }
        const uint64_t frameEndedUs = PerfNowUs();
        loopPerf.NoteWork(frameEndedUs - frameStartedUs);
        loopPerf.NotePresent(swapOk, frameEndedUs);
        if (zeroCopySceneReady && zeroCopyScene_.diagnosticSerial &&
            (loopDiagnostics || FrameTraceEnabled()))
            OH_LOG_INFO(LOG_APP, "[SCENE-SWAP] sample=%{public}llu at_us=%{public}llu ok=%{public}d",
                static_cast<unsigned long long>(zeroCopyScene_.diagnosticSerial),
                static_cast<unsigned long long>(frameEndedUs), swapOk);
        if (haveFrame) {
            // 诊断: 每帧有帧 swap 都打印 — 对齐合成(MW-TAKE)时刻与上屏(swap)时刻,
            // skip 累计 = 自上次上屏以来跳过多少次无帧循环 (帧被延迟多久)。
            // 默认关闭 (WINEHUA_FRAME_TRACE=1 开启, 见 perf_utils.h)
            if (FrameTraceEnabled()) {
                OH_LOG_INFO(LOG_APP,
                            "[MW-SWAP] tl=%{public}u loop=%{public}llu f=%{public}d/%{public}d/%{public}d skip=%{public}llu take=%{public}lluus swap=%{public}lluus",
                            useToplevel, static_cast<unsigned long long>(loopCount),
                            cpuFrame ? 1 : 0, zeroCopyFrame ? 1 : 0, zeroCopyGeometryFrame ? 1 : 0,
                            static_cast<unsigned long long>(skipFrames_), static_cast<unsigned long long>(takeUs),
                            static_cast<unsigned long long>(frameEndedUs - swapStartedUs));
            }
            skipFrames_ = 0;
            perf.Add(useToplevel, takeUs, uploads.cpuUs, frameEndedUs - swapStartedUs,
                     frameEndedUs - frameStartedUs, uploads.bytes, swapOk, uploads.calls, uploads.timeEnabled);
        }
        fps.Tick();
        loopCount++;
        if (!waitForFrameTick()) break;
    }

    directDesktop.reset();
    ShutdownZeroCopyConsumer();
    if (nativeVsync) OH_NativeVSync_Destroy(nativeVsync);
}

void EglRenderer::Shutdown() {
    running_ = false;
    vsyncCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // Destroy the Vulkan surface before its NativeWindow; Shutdown is idempotent.
    vulkanDesktop_.reset();
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
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Shutdown OK (display retained)", toplevelId_.load(std::memory_order_relaxed));
    }
    display_ = EGL_NO_DISPLAY;
    // surfaceId 创建的 native window 在这里销毁 (EglRenderer 持有 window_ 指针)
    if (window_) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u native window destroyed", toplevelId_.load(std::memory_order_relaxed));
    }
}

bool EglRenderer::RootCompositing() const {
    // Desktop policy alone cannot promote an independent Linux carrier to
    // the Wine root when Wine is prepared later in the same app process.
    const uint32_t id = GetToplevelId();
    return id && id == compositor_.DesktopRootToplevelId() &&
        !isolatedToplevel_ && compositor_.Policy().RootCompositing();
}
