#pragma once
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include "compositor/geometry.h"
#include "compositor/presented_frame.h"

struct OH_NativeImage;

// 最小 EGL 渲染器: 从 WaylandServer 取帧 -> GL 纹理 -> XComponent 上屏
// 所有实例共享同一个 EGLDisplay (避免反复 init/terminate 导致 GPU 驱动竞争)
// 每个实例拥有独立的 EGLContext + EGLSurface
class EglRenderer {
public:
    // 获取/初始化共享的 EGLDisplay (首次调用时初始化, 线程安全)
    static EGLDisplay GetSharedDisplay();

    bool Init(OHNativeWindow* window, int w, int h);
    void Shutdown();

    uint32_t GetToplevelId() const { return toplevelId_; }
    void SetToplevelId(uint32_t id) { toplevelId_ = id; }
    void SetSize(int w, int h) { width_ = w; height_ = h; sizeDirty_.store(true); }
    bool IsValid() const { return running_; }

    // 尺寸 getters (供输入坐标转换: 触控坐标 -> wine 内容坐标)
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    int GetFrameWidth() const { return frameW_; }
    int GetFrameHeight() const { return frameH_; }
    // 输入逆映射锚 (PresentedFrame 契约, 重构第 2B 步): 帧坐标空间的逻辑
    // 内容尺寸 (contentW/H) 到当前 surface 的保比例 fit — 桌面合成/快进帧
    // 锚定 root 逻辑尺寸, SHM 直传帧同样锚定桌面尺寸 (与显示 letterbox 的
    // buffer 尺寸锚解耦, 红警2 直传点击修复的契约化); PC 窗口帧锚定窗口
    // 内容尺寸 (= 显示 letterbox, content == buffer)。锚未就绪 (首帧前)
    // 或 fit 失败时退回显示 letterbox (与旧 CoordTransform fallback 一致)。
    FitRect GetInputLetterbox() const;

private:
    void RenderLoop();
    static void OnVSync(long long timestamp, void* data);
    static void OnZeroCopyFrameAvailable(void* data);
    bool InitZeroCopyConsumer();
    bool TryAttachZeroCopySurface(uint32_t rendererToplevelId);
    bool UpdateZeroCopyFrame(int& width, int& height);
    void ReleaseZeroCopyBinding();
    void ShutdownZeroCopyConsumer();

    // -- ZC 状态发布点收敛 (阶段 3) --
    // 三处状态 (compositor zeroCopySurfaceKeys_ / broker ready marker /
    // renderer 内部 zeroCopyReadyPublished_) 的全部更新收敛到这三个方法,
    // 每个幂等 (发布过才撤销)。时序设计: 发布时先 compositor key 后 ready
    // (先让合成跳过, 再通知 guest 走 ZC); fallback 分两步 — 先撤 ready
    // (guest 立即切 SHM), 等 shmCommitSerial 越过基线 (新 SHM 帧已到) 再
    // 撤 compositor key (恢复合成), 避免合成到 ZC 前的旧 SHM 帧。broker 的
    // attached 集合 (IPC 簿记) 由 Attach/DetachZeroCopyTarget 独立维护,
    // 不参与合成判定 (CompositorLayer::zcActive 是唯一消费字段)。
    void PublishZeroCopyActive(uint32_t rendererToplevelId);
    void UnpublishZeroCopyReady(uint32_t rendererToplevelId);
    void ClearZeroCopyCompositorKey();

    OHNativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    GLuint texture_ = 0;
    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLuint occluderVbo_ = 0;  // desktop 模式 zero-copy 遮挡区域重绘 (动态 UV quad)
    OH_NativeImage* zeroCopyImage_ = nullptr;
    OHNativeWindow* zeroCopyProducerWindow_ = nullptr;
    GLuint zeroCopyTexture_ = 0;
    GLuint zeroCopyProgram_ = 0;
    GLint zeroCopyTransformLocation_ = -1;
    std::atomic<bool> zeroCopyFrameAvailable_{false};
    std::atomic<uint64_t> zeroCopyFrameSignals_{0};
    uint64_t zeroCopyFrames_ = 0;
    uint64_t zeroCopyUpdates_ = 0;
    uint64_t zeroCopyLastConsumedSignal_ = 0;
    uint64_t zeroCopyCoalescedSignals_ = 0;
    uint64_t zeroCopyDuplicateTimestamps_ = 0;
    uint64_t zeroCopyFailures_ = 0;
    uint64_t zeroCopyFallbackShmSerial_ = 0;
    uint64_t zeroCopyTimestampRegressions_ = 0;
    int64_t zeroCopyLastTimestamp_ = 0;
    uint64_t zeroCopySurfaceKey_ = 0;
    uint64_t zeroCopyLastQueryUs_ = 0;
    uint32_t zeroCopyClientPid_ = 0;
    uint32_t zeroCopySurfaceId_ = 0;
    int zeroCopySourceW_ = 0;
    int zeroCopySourceH_ = 0;
    int zeroCopyLayerX_ = 0;
    int zeroCopyLayerY_ = 0;
    int zeroCopyLayerW_ = 0;
    int zeroCopyLayerH_ = 0;
    bool zeroCopyRegistered_ = false;
    // 无新帧跳过 swap 的累计次数 (诊断: 帧合成后多久没上屏)
    uint64_t skipFrames_ = 0;
    bool zeroCopyListenerSet_ = false;
    bool zeroCopyReadyPublished_ = false;
    bool zeroCopyHasFrame_ = false;
    bool zeroCopyVulkanSource_ = false;
    bool zeroCopyFallbackPending_ = false;
    bool zeroCopyGeometryDirty_ = false;
    bool zeroCopyFullscreen_ = false;  // 所属 toplevel 全屏: ZC 层保比例铺满显示区
    uint32_t zeroCopyConsecutiveFailures_ = 0;
    float zeroCopyTransform_[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    float zeroCopySamplingTransform_[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    int width_ = 0, height_ = 0;
    std::atomic<bool> sizeDirty_{false};  // resize(SetSize)后需强制重绘一次(无新帧也上屏), 避免旧帧被拉伸
    int frameW_ = 0, frameH_ = 0;  // Wine 帧内容尺寸 (坐标转换)
    bool frameArgb_ = false;       // 当前帧是 ARGB8888 (layered/shaped 异型窗口, 透传 alpha)
    int texW_ = 0, texH_ = 0;      // 上次上传的纹理尺寸 (用于避免每帧 glTexImage2D)
    FitRect letterbox_;  // 显示 letterbox: buffer 尺寸 (frame.w/h) 到 surface 的保比例 fit
    // 输入逆映射锚 (PresentedFrame 契约, 重构第 2B 步): 最近一帧契约的 contentW/H
    // (逻辑内容尺寸)。桌面合成/快进/直传帧 = root 逻辑尺寸 (与 buffer 尺寸解耦,
    // 直传游戏帧 buffer 800x600 但 content 仍是桌面 1400x920 — 红警2 修复点);
    // PC 窗口帧 = 窗口内容尺寸 (content == buffer)。GetInputLetterbox 用它对当前
    // surface 做保比例 fit; 无帧 (contentW/H=0) 或 fit 失败退回显示 letterbox_。
    int contentW_ = 0, contentH_ = 0;
    int bufW_ = 0, bufH_ = 0;  // 上次 SET_BUFFER_GEOMETRY 的值, 避免重复调用
    int lastLoggedW_ = 0, lastLoggedH_ = 0;  // 上次输出 resize 日志时的 surface 尺寸
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex vsyncMutex_;
    std::condition_variable vsyncCv_;
    uint64_t vsyncSequence_ = 0;
    std::atomic<long long> vsyncPeriodNs_{16666667};

    uint32_t toplevelId_ = 0;
};
