#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "compositor/compositor_constants.h"
#include "compositor/display_policy.h"
#include "compositor/toplevel_manager.h"

// 前置声明: surface_commit 分段函数的参数类型 (定义在 compositor/surface_data.h,
// 该头在文件末尾引入 — 本类的签名只需引用, 无需完整定义)
struct SurfaceData;
struct ShmCommitInfo;
#include "compositor/move_grab.h"
#include "compositor/desktop_compositor.h"
#include "compositor/input_resolver.h"
#include "compositor/desktop_root_manager.h"
#include "plugin_manager.h"  // PromotePendingDesktopRoot → MoveRendererToToplevel

// 最小 Wayland Compositor: wl_compositor + wl_surface + wl_shm
class WaylandServer {
public:
    // Re-export types moved to compositor/ (backward compat)
    using ZeroCopyLayerInfo = ::ZeroCopyLayerInfo;
    using ZeroCopyOccluderRect = ::ZeroCopyOccluderRect;
    using SubsurfaceLayer = DesktopCompositor::SubsurfaceLayer;
    using InputTarget = ::InputTarget;

    using StateCb = std::function<void(const char*)>;
    // toplevel 回调: (toplevelId, eventName, jsonData)
    // events: "created", "destroyed", "title", "configure"
    using ToplevelCb = std::function<void(uint32_t, const char*, const char*)>;

    static WaylandServer* GetInstance();

    wl_display* GetDisplay() const { return display_; }

    bool Start(const std::string& socketPath);
    void Stop();

    // EglRenderer 调用: 取最新一帧像素 (deprecated, 用 TakeToplevelFrame)
    bool TakeFrame(std::vector<uint8_t>& outPixels, int& w, int& h);
    bool TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h) {
        return desktopCompositor_.TakeToplevelFrame(id, out, w, h);
    }
    bool GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                              int fallbackWidth, int fallbackHeight,
                              ZeroCopyLayerInfo& info) {
        return desktopCompositor_.GetZeroCopyLayerInfo(
            surfaceKey, rendererToplevelId, fallbackWidth, fallbackHeight, info);
    }
    void SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled) {
        desktopCompositor_.SetSurfaceZeroCopy(surfaceKey, enabled);
    }
    int GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                             ZeroCopyOccluderRect* out, int maxOut) {
        return desktopCompositor_.GetZeroCopyOccluders(surfaceKey, rendererToplevelId, out, maxOut);
    }

    // 状态回调 (首帧到达 -> 通知 ArkTS)
    void SetStateCallback(StateCb cb) { stateCb_ = std::move(cb); }
    void FireState(const char* s) { if (stateCb_) stateCb_(s); }
    // 会话内 root 切换防御: 新 root 出现时重置首帧标记 (pending root 机制下
    // 旧 root 可能未销毁, 会话结束的 ResetSessionState 不会触发), 让新 root
    // 首帧重新注入 focus。见 CheckDesktopRootOnCommit 的 fireDesktopRoot 分支
    void ResetFirstFrame() { firstFrame_ = false; }
    // Wine 会话终结统一收口 (desktop root 销毁 / stopClient 时调用):
    // 重置进程级一次性状态 (firstFrame_/输入焦点/按键/修饰键), 使下一次
    // 引擎启动 (冷/热) 从与冷启动一致的基线开始 — 状态生命周期按「Wine
    // 会话」建模, 而非按「进程」建模。热重启复用同一进程/同一 WaylandServer,
    // 任何只初始化一次的字段都会跨会话残留 (firstFrame_ 曾导致热重启桌面
    // 永不激活)。注意: 调用方须不在 toplevelMgr_ 锁内 (EndMoveGrab/Input 会拿锁)
    void ResetSessionState();

    // toplevel 回调 (xdg_toplevel 生命周期 -> 通知 ArkTS 创建/销毁窗口)
    void SetToplevelCallback(ToplevelCb cb) { toplevelCb_ = std::move(cb); }
    void FireToplevelEvent(uint32_t id, const char* event, const char* jsonData = "{}");

    // 生成唯一 toplevel ID
    uint32_t NextToplevelId() { return toplevelMgr_.AllocateToplevelId(); }

    // toplevel resource 映射 (用于 SendToplevelClose -> xdg_toplevel_send_close)
    void RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl);
    void UnregisterToplevelResource(uint32_t toplevelId);
    // 清理 toplevel 像素数据 + 标记 root dirty (desktop mode)
    void OnToplevelDestroyed(uint32_t toplevelId);
    void SendToplevelClose(uint32_t toplevelId);
    // 统一状态转换 (确保 minimize/maximize/restore 涉及的 map 操作原子化)
    void SetToplevelMinimized(uint32_t id);
    void SetToplevelRestored(uint32_t id);
    void SetToplevelMaximized(uint32_t id);
    // 全屏状态登记 (desktop 合成按保比例缩放+黑边绘制, 输入按同一变换逆映射)
    void SetToplevelFullscreen(uint32_t id, bool on);
    // surface 尺寸变化后强制下次渲染循环取帧重绘 (避免旧 viewport 贴新 surface 导致黑边)
    void ForceToplevelRedraw(uint32_t id);
    // 旧接口 → 转发到新方法
    void NotifyWindowRestored(uint32_t id) { SetToplevelRestored(id); }
    void NotifyToplevelMinimized(uint32_t id, int32_t, int32_t) { SetToplevelMinimized(id); }
    // 鸿蒙侧 surface 尺寸变化时调用: 发 configure 通知 Wine 用新尺寸渲染
    void NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h);
    // 设置输出尺寸 (替换硬编码 1280x720)
    void SetOutputSize(int32_t w, int32_t h) { outputW_ = w; outputH_ = h; }
    int32_t outputW_ = compositor_consts::kDefaultOutputWidth;
    int32_t outputH_ = compositor_consts::kDefaultOutputHeight;
    int32_t GetWorkAreaHeight();  // 排除任务栏后的可用高度
    uint32_t FindToplevelAt(int x, int y) { return inputResolver_.FindToplevelAt(x, y); }
    bool FindInputTargetAt(int x, int y, InputTarget& out) {
        return inputResolver_.FindInputTargetAt(x, y, out);
    }
    bool IsSurfaceAlive(wl_resource* surface) { return inputResolver_.IsSurfaceAlive(surface); }
    // warp 补偿门控/锚点换算 (wp_pointer_warp_v1 → InputManager::OnPointerWarp)
    bool IsSurfaceFromZcGame(wl_resource* surface) { return inputResolver_.IsZcGameSurface(surface); }
    bool SurfaceLocalToDesktop(wl_resource* surface, double lx, double ly, double& dx, double& dy) {
        return inputResolver_.SurfaceLocalToDesktop(surface, lx, ly, dx, dy);
    }
    // Desktop 模式: 提到 Z-order 最顶层。
    // userInitiated=true 仅用于用户显式操作路径 (ArkTS 任务栏/窗口点击),
    // 会对已 fullscreen 的目标重新取全屏优先级号; tl_set_fullscreen 等
    // 批处理路径必须保持默认 false。见 ToplevelState::fsPriority 注释
    void RaiseToplevel(uint32_t id, bool userInitiated = false);
    // 读取 toplevel 桌面坐标 (InputManager 坐标转换用)
    // miss 返回 0 (与旧实现返回值一致), find 语义无插入副作用
    int GetToplevelX(uint32_t id) { return toplevelMgr_.GetToplevelX(id); }
    int GetToplevelY(uint32_t id) { return toplevelMgr_.GetToplevelY(id); }
    int GetToplevelW(uint32_t id) { return toplevelMgr_.GetToplevelW(id); }
    int GetToplevelH(uint32_t id) { return toplevelMgr_.GetToplevelH(id); }
    // 状态查询 (minimized/fullscreen 权威字段在 ToplevelState, 见 surface_data.h 状态边界注释)
    bool IsToplevelMinimized(uint32_t id) { return toplevelMgr_.IsToplevelMinimized(id); }
    bool IsToplevelFullscreen(uint32_t id) { return toplevelMgr_.IsToplevelFullscreen(id); }
    // toplevel/popup 帧的 wl_shm 格式 (0=ARGB8888 有意义 alpha, 1=XRGB8888, 默认 1)
    // EglRenderer 据此决定 alpha 透传或强制不透明 (XRGB 的 X 字节是垃圾)
    uint32_t GetToplevelShmFormat(uint32_t id) { return toplevelMgr_.GetToplevelShmFormat(id); }
    // ARGB 异型窗口的 0/1 剪影掩码 (setWindowMask 用, ArkTS 轮询拉取)
    using WindowMask = ToplevelManager::WindowMask;
    // 取掩码: false = 无掩码或无更新; 取走清除 dirty
    bool TakeWindowMask(uint32_t id, int& w, int& h, std::vector<uint8_t>& out);
    // Desktop 合成模式 (Tablet): 全部 toplevel 合成到一个 root framebuffer。
    // 模式差异的策略查询走 Policy() (display_policy.h); IsDesktopMode 只用于
    // 模式上报类调用 (给 wine 传环境/标记进程/日志)。
    void SetDesktopMode(bool on) { policy_ = DisplayPolicy::FromDesktopMode(on); }
    bool IsDesktopMode() const { return policy_.desktop; }
    const DisplayPolicy& Policy() const { return policy_; }
    void SetDesktopRootToplevelId(uint32_t id) { desktopRootToplevelId_ = id; }
    uint32_t GetDesktopRootToplevelId() const { return desktopRootToplevelId_; }
    void SetDesktopRootRecognitionEnabled(bool enabled) { desktopRootMgr_.SetRecognitionEnabled(enabled); }
    void PromotePendingDesktopRoot() {
        uint32_t id = desktopRootMgr_.PromotePending();
        if (id) PluginManager::GetInstance()->MoveRendererToToplevel(0, id);
    }

    // -- wayland 协议实现 --
    static void compositor_bind(wl_client*, void*, uint32_t, uint32_t);
    static void compositor_create_surface(wl_client*, wl_resource*, uint32_t);
    static void compositor_create_region(wl_client*, wl_resource*, uint32_t);

    static void surface_destroy(wl_client*, wl_resource*);
    static void surface_attach(wl_client*, wl_resource*, wl_resource*, int32_t, int32_t);
    static void surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t);
    static void surface_frame(wl_client*, wl_resource*, uint32_t);
    static void surface_commit(wl_client*, wl_resource*);
    static void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
    static void surface_set_input_region(wl_client*, wl_resource*, wl_resource*);
    static void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
    static void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
    static void surface_damage_buffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
    static void surface_offset(wl_client*, wl_resource*, int32_t, int32_t) {}

    static void region_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void region_add(wl_client*, wl_resource* r, int32_t, int32_t, int32_t, int32_t) {
        int* count = static_cast<int*>(wl_resource_get_user_data(r));
        if (count) (*count)++;
    }
    static void region_subtract(wl_client*, wl_resource* r, int32_t, int32_t, int32_t, int32_t) {
        // 追踪计数: Wine 只用空/非空判断
        int* count = static_cast<int*>(wl_resource_get_user_data(r));
        if (count) (*count)++;
    }

    /* wl_subcompositor */
    static void subcompositor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void subcompositor_get_subsurface(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*);
    /* wl_subsurface */
    static void subsurface_destroy(wl_client*, wl_resource* r);
    static void subsurface_set_position(wl_client*, wl_resource*, int32_t, int32_t);
    static void subsurface_place_above(wl_client*, wl_resource*, wl_resource*);
    static void subsurface_place_below(wl_client*, wl_resource*, wl_resource*);
    static void subsurface_set_sync(wl_client*, wl_resource*) {}
    static void subsurface_set_desync(wl_client*, wl_resource*) {}

    /* wp_viewporter */
    static void viewporter_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void viewporter_get_viewport(wl_client*, wl_resource*, uint32_t, wl_resource*);
    /* wp_viewport */
    static void viewport_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void viewport_set_source(wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t);
    static void viewport_set_destination(wl_client*, wl_resource*, int32_t, int32_t);

    /* Globals bind */
    static void subcompositor_bind(wl_client*, void*, uint32_t, uint32_t);
    static void viewporter_bind(wl_client*, void*, uint32_t, uint32_t);
    static void output_bind(wl_client*, void*, uint32_t, uint32_t);
    /* wl_output */
    static void output_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

    // toplevelId -> wl_surface 映射 (供 Seat::InjectPointerEnter 查找)
    wl_resource* GetSurfaceForToplevel(uint32_t toplevelId);

    // 交互式窗口移动 (xdg_toplevel.move) — 由 xdg_shell 和 InputManager 调用
    bool IsMoveGrabActive() const { return moveGrab_.IsActive(); }
    uint32_t GetMoveGrabToplevelId() const { return moveGrab_.GetToplevelId(); }
    void StartMoveGrab(uint32_t toplevelId, uint32_t serial);
    void EndMoveGrab();
    bool ProcessMoveGrabMotion(int32_t gx, int32_t gy);

private:
    WaylandServer() = default;
    void EventLoop();

    // -- surface_commit 分段 (Phase 3B, 实现在 wl_core.cpp) --
    // 协议语义见各函数定义处注释; ShmCommitInfo 在 surface_data.h
    bool HandleNullBufferCommit(SurfaceData* sd, wl_resource* surfRes);
    bool BeginShmAccess(SurfaceData* sd, ShmCommitInfo& fi);
    void ComputeContentArea(SurfaceData* sd, ShmCommitInfo& fi);

    void UpdateToplevelFrameOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                     ShmCommitInfo& fi, bool& outFirstCommit);
    void CheckDesktopRootOnCommit(SurfaceData* sd, ShmCommitInfo& fi, bool isFirstCommit);
    void UpdateSubsurfaceOnCommit(SurfaceData* sd, wl_resource* surfRes, ShmCommitInfo& fi);
    void UpdateSubsurfaceLayerOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                       uint32_t parentId, ShmCommitInfo& fi);
    void UpdatePopupOnCommit(SurfaceData* sd, wl_resource* surfRes,
                             SurfaceData* parentSd, ShmCommitInfo& fi);
    void FinishCommit(SurfaceData* sd, wl_resource* surfRes);

    wl_display* display_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // 全局帧缓冲 (deprecated, 保留兼容)
    std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    int width_ = 0, height_ = 0;
    std::atomic<bool> dirty_{false};

    // toplevel 状态存储已移入 ToplevelManager (compositor/toplevel_manager.h)
    ToplevelManager toplevelMgr_;

    void MarkDesktopRootDirtyLocked() { desktopRootMgr_.MarkRootDirtyLocked(); }

    StateCb stateCb_;
    ToplevelCb toplevelCb_;
    std::atomic<bool> firstFrame_{false};

    // Desktop 合成模式策略 (唯一模式存储位, DesktopCompositor 持引用随动)
    DisplayPolicy policy_{};
    uint32_t desktopRootToplevelId_ = 0;
    uint32_t pendingDesktopRootToplevelId_ = 0;
    uint32_t taskbarId_ = 0;  // app_id == "explorer.exe.taskbar", RaiseToplevel/GetWorkAreaHeight 用
    bool desktopRootRecognitionEnabled_ = true;
    // 交互式窗口移动 (xdg_toplevel.move) — 已移入 MoveGrabHandler
    MoveGrabHandler moveGrab_;
    // 桌面 Root 识别+切换 — 已移入 DesktopRootManager
    DesktopRootManager desktopRootMgr_{toplevelMgr_, desktopRootToplevelId_,
                                        pendingDesktopRootToplevelId_,
                                        desktopRootRecognitionEnabled_,
                                        outputW_, outputH_,
                                        [this](uint32_t id, const char* ev, const char* data) {
                                            FireToplevelEvent(id, ev, data);
                                        }};
    // 帧合成 + zero-copy layer 管理 — 已移入 DesktopCompositor
    DesktopCompositor desktopCompositor_{toplevelMgr_, policy_, desktopRootToplevelId_,
                                          outputW_, outputH_};
    // 输入命中裁决 — 已移入 InputResolver
    InputResolver inputResolver_{toplevelMgr_, desktopCompositor_, desktopRootToplevelId_,
                                  outputW_, outputH_};

    // 渲染/输入共用的 toplevel 可见性检查 (调用方须已持有锁)
    bool IsToplevelVisibleLocked(uint32_t id) {
        return toplevelMgr_.IsToplevelVisibleLocked(id, desktopRootToplevelId_);
    }
};

#include "compositor/surface_data.h"  // SurfaceData 已提取至独立头文件
