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
#include "compositor/frame/geometry.h"
#include "compositor/frame/presented_frame.h"
#include "compositor/frame/direct_pass_policy.h"  // DirectPassPolicy (直传能力位接口, 任务 3)

struct OH_NativeImage;
class DesktopCompositor;

// 最小 EGL 渲染器: 从 DesktopCompositor 取帧 -> GL 纹理 -> XComponent 上屏
// 所有实例共享同一个 EGLDisplay (避免反复 init/terminate 导致 GPU 驱动竞争)
// 每个实例拥有独立的 EGLContext + EGLSurface
class EglRenderer : public winehua::DirectPassPolicy {
public:
    // 构造注入 frame compositor (重构第 6A 步): 取帧/ZC 层几何与状态机经
    // DesktopCompositor 直连 — 替代旧 WaylandServer 门面的一行转发
    // (TakeToplevelFrame/GetZeroCopyLayerInfo/ActivateZcSurface 等 26 处调用)。
    // 注入点 = PluginManager::CreateRenderer (WaylandServer::GetDesktopCompositor)。
    // compositor 生命周期长于一切 renderer (WaylandServer 单例成员), 只读
    // 引用共享与 InputResolver/PopupManager 注入同模式, 无新锁。
    explicit EglRenderer(DesktopCompositor& compositor);

    // 获取/初始化共享的 EGLDisplay (首次调用时初始化, 线程安全)
    static EGLDisplay GetSharedDisplay();

    bool Init(OHNativeWindow* window, int w, int h);
    void Shutdown();

    uint32_t GetToplevelId() const { return toplevelId_; }
    void SetToplevelId(uint32_t id) { toplevelId_ = id; }
    // ArkTS onSurfaceChanged 声明的 surface 尺寸 (只作诊断基准, 见成员区注释)。
    // **不写 width_/height_**: 那两个字段是"实测 surface 尺寸", 被声明值污染后
    // 循环里的"实测 == 已画"判定会误判 (2026-09-14 黑边根因)
    void SetSize(int w, int h) {
        expectW_ = w; expectH_ = h;
    }
    // 拖拽缩放中: 整帧拉伸填满 surface (见 ComputeFrameDisplayRect)。拖拽时
    // 窗口先变、Wine 新帧未到的空档里, 等比 fit 会按旧帧比例留黑边; 拉伸让
    // 旧帧先铺满, 新帧到达自然消除。由 WaylandServer::NotifyToplevelResize
    // 随 configure 同步 (拖拽结束的 0 尺寸 configure 带 resizing=false 清掉)。
    void SetStretchFill(bool on) { stretchFill_.store(on); }
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
    // 直传能力位 (DirectPassPolicy, 任务 3): 渲染器 GL 行为声明 —
    // uForceOpaque/无 GL_BLEND/fit 同源/XRGB 不透明, 恒全备 (来源见 .cpp 实现)
    uint32_t DirectPassCapabilities() const override;

private:
    void RenderLoop();
    static void OnVSync(long long timestamp, void* data);
    static void OnZeroCopyFrameAvailable(void* data);
    bool InitZeroCopyConsumer();
    bool TryAttachZeroCopySurface(uint32_t rendererToplevelId);
    bool UpdateZeroCopyFrame(int& width, int& height);
    void ReleaseZeroCopyBinding();
    void ShutdownZeroCopyConsumer();

    // 整帧的显示矩形 (surface 坐标): 常态 = letterbox_ (等比 fit); 拖拽缩放中
    // = 填满 surface (SetStretchFill)。letterbox_ 本身保持等比映射锚语义不变 —
    // 帧内坐标映射 (ZC 层/遮挡重绘/输入逆映射) 都锚它, 本矩形只服务"整帧显示"。
    FitRect ComputeFrameDisplayRect(int drawW, int drawH) const;

    OHNativeWindow* window_ = nullptr;
    // 沉浸式切换的两拍 resize (2800x1683 → 2800x1840) 与渲染循环的竞态
    // (2026-09-14 全屏桌面左右黑边 + 画面纵向拉伸根因, 日志实证):
    // 渲染线程按当时读到的 surface (1683) 算出 letterbox 绘制; 绘制期间 NAPI
    // 线程的第二拍 SetSize(1840) 到达, 旧实现把 width_/height_ 一并改写为声明值。
    // 系统随后把 buffer 切到 1840, 而"实测 surface == width_"的跳过判定因为
    // width_ 已是 1840 而成立 → 永不重绘, 上屏的 2561x1683 旧画面被系统非等比
    // 拉伸, 且无新帧不会自愈。
    // 修正: 跳过判定改用 lastDrawW_/lastDrawH_ ("上次真正画上去的尺寸"), 且
    // SetSize 不再改写 width_/height_ (保持"实测 surface 尺寸"的单一语义)。
    // surface 什么时候真的变了就什么时候重绘 — 天然收敛, 不需要代际/重试状态机。
    int expectW_ = 0, expectH_ = 0;      // ArkTS 声明的尺寸 (只作诊断告警基准)
    int lastDrawW_ = 0, lastDrawH_ = 0;  // 上次成功上屏的绘制尺寸 (跳过判定的唯一依据)
    // 诊断限频: 只在数值变化时打印, 避免"尺寸长期不匹配"的稳态每帧刷屏
    int lastWarnSurfW_ = 0, lastWarnSurfH_ = 0;
    int lastFitLogW_ = 0, lastFitLogH_ = 0, lastFitLogFw_ = 0, lastFitLogFh_ = 0;
    int lastFitLogLbW_ = 0, lastFitLogLbH_ = 0;
    int swapFailStreak_ = 0;             // eglSwapBuffers 连续失败次数 (诊断)
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
    bool zeroCopyHasFrame_ = false;
    bool zeroCopyVulkanSource_ = false;
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

    int width_ = 0, height_ = 0;   // 实测 surface 尺寸 (每轮 eglQuerySurface 刷新, 只此一处语义)
    int frameW_ = 0, frameH_ = 0;  // Wine 帧内容尺寸 (坐标转换)
    bool frameArgb_ = false;       // 当前帧是 ARGB8888 (layered/shaped 异型窗口, 透传 alpha)
    int texW_ = 0, texH_ = 0;      // 上次上传的纹理尺寸 (用于避免每帧 glTexImage2D)
    FitRect letterbox_;  // 等比映射锚: buffer 尺寸 (frame.w/h) 到 surface 的保比例 fit
    // 拖拽缩放中: 整帧拉伸填满 (见 SetStretchFill / ComputeFrameDisplayRect)
    std::atomic<bool> stretchFill_{false};
    // 输入逆映射锚 (PresentedFrame 契约, 重构第 2B 步): 最近一帧契约的 contentW/H
    // (逻辑内容尺寸)。桌面合成/快进/直传帧 = root 逻辑尺寸 (与 buffer 尺寸解耦,
    // 直传游戏帧 buffer 800x600 但 content 仍是桌面 1400x920 — 红警2 修复点);
    // PC 窗口帧 = 窗口内容尺寸 (content == buffer)。GetInputLetterbox 用它对当前
    // surface 做保比例 fit; 无帧 (contentW/H=0) 或 fit 失败退回显示 letterbox_。
    int contentW_ = 0, contentH_ = 0;
    int lastLoggedW_ = 0, lastLoggedH_ = 0;  // 上次输出 resize 日志时的 surface 尺寸
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex vsyncMutex_;
    std::condition_variable vsyncCv_;
    uint64_t vsyncSequence_ = 0;
    std::atomic<long long> vsyncPeriodNs_{16666667};

    uint32_t toplevelId_ = 0;

    // frame compositor 引用 (构造注入, 见构造函数注释): 取帧/层几何/ZC
    // 状态机直连目标 — 渲染线程唯一需要的外部 compositor 入口。
    DesktopCompositor& compositor_;
};
