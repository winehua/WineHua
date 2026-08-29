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
#include "compositor/popup_manager.h"
#include "compositor/toplevel_event_bus.h"  // ToplevelEventType/ToplevelEventBus (重构第 5D 步)

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
    // (消费端子集见 napi_init.cpp SetToplevelCallback; 事件名/JSON 构造收口
    // 于 ToplevelEventBus — 重构第 5D 步, 本签名未变)
    using ToplevelCb = std::function<void(uint32_t, const char*, const char*)>;

    static WaylandServer* GetInstance();

    wl_display* GetDisplay() const { return display_; }

    // -- 子组件装配出口 (重构第 6A 步, 与 GetDisplay 同模式) --
    // 业务组件 (EglRenderer / InputManager / PointerExtras / xdg_shell 等)
    // 经此取得子组件引用做构造/装配注入, 替代本类到业务方法的转发 —
    // 本类不再转发业务方法 (处置表见 docs/COMPOSITOR_REFACTOR_STATUS.md §二 6A)。
    // 引用与成员同生命周期 (单例启动前即构造), 装配在 wl 事件循环启动前
    // (与 4C1 warpSink / 5D bus 装配同模式, 无新锁)。
    ToplevelManager& GetToplevelManager() { return toplevelMgr_; }
    DesktopCompositor& GetDesktopCompositor() { return desktopCompositor_; }
    // root 身份的共享引用装配出口 (与 DesktopCompositor/InputResolver 注入的
    // 引用同源, 6-B DesktopSessionState 化后换成 POD 成员)
    const uint32_t& DesktopRootToplevelIdRef() const { return desktopRootToplevelId_; }

    bool Start(const std::string& socketPath);
    void Stop();

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

    // toplevel 回调 (xdg_toplevel 生命周期 -> 通知 ArkTS 创建/销毁窗口):
    // 实现 = ToplevelEventBus::SetEventSink (重构第 5D 步, bus 收口事件通道,
    // 签名/装配点不变 — napi_init.cpp SetToplevelCallback 零改动)
    void SetToplevelCallback(ToplevelCb cb) { toplevelEventBus_.SetEventSink(std::move(cb)); }
    /* toplevel 事件投递 (重构第 5D 步收口点): 事件名 enum 化 + JSON 由
     * ToplevelEventBus::Json* 构造单点 + NAPI 通道移出 compositor 核心 —
     * 本函数语义 = 旧 FireToplevelEvent (bus 投递 + desktop_root 会话侧旁路,
     * 见 wayland_server.cpp)。事件名/JSON/日志逐字不变 (红线)。 */
    void PostToplevelEvent(uint32_t id, ToplevelEventType evt,
                           const std::string& json = "{}");

    // toplevel resource 映射 (用于 SendToplevelClose -> xdg_toplevel_send_close)
    void RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl);
    void UnregisterToplevelResource(uint32_t toplevelId);
    // 清理 toplevel 像素数据 + 标记 root dirty (desktop mode)
    void OnToplevelDestroyed(uint32_t toplevelId);
    void SendToplevelClose(uint32_t toplevelId);
    // 统一状态转换 (确保 minimize/maximize/restore 涉及的 map 操作原子化)
    void SetToplevelMinimized(uint32_t id);
    void SetToplevelRestored(uint32_t id);
    // 最大化生效的合成器反应: 锚定桌面原点 (最大化窗口全屏尺寸铺满) + dirty。
    // 注意: 不写 maximized 状态位 — 状态位经 SetToplevelMaximizedState
    // (重构第 5C 步: maximized 权威迁入 ToplevelState, 本函数与状态位分离
    // 是历史形态, 只做几何反应)。
    void SetToplevelMaximized(uint32_t id);
    // maximized 状态位写 (xdg_shell 协议处理调用): Ensure 建档 + 裸状态赋值,
    // 无日志无 dirty — 对齐旧 sd->maximized 直接赋值的语义 (dirty 由调用点
    // 随后的 SetToplevelMaximized 锚定 / configure 路径负责)。重构第 5C 步。
    void SetToplevelMaximizedState(uint32_t id, bool on);
    // 全屏状态登记 (desktop 合成按保比例缩放+黑边绘制, 输入按同一变换逆映射)
    void SetToplevelFullscreen(uint32_t id, bool on);
    // surface 尺寸变化后强制下次渲染循环取帧重绘 (避免旧 viewport 贴新 surface 导致黑边)
    void ForceToplevelRedraw(uint32_t id);
    // 鸿蒙侧 surface 尺寸变化时调用: 发 configure 通知 Wine 用新尺寸渲染
    void NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h);
    // 设置输出尺寸 (替换硬编码 1280x720)
    void SetOutputSize(int32_t w, int32_t h) { outputW_ = w; outputH_ = h; }
    int32_t outputW_ = compositor_consts::kDefaultOutputWidth;
    int32_t outputH_ = compositor_consts::kDefaultOutputHeight;
    int32_t GetWorkAreaHeight();  // 排除任务栏后的可用高度
    // 输入命中顶层 toplevel 查询 (NAPI 入口 findToplevelAt 唯一调用方 —
    // 入口模块保留委托; 业务层 InputManager 已改直呼 InputResolver,
    // 重构第 6A 步)
    uint32_t FindToplevelAt(int x, int y) { return inputResolver_.FindToplevelAt(x, y); }
    // Desktop 模式: 提到 Z-order 最顶层。
    // userInitiated=true 仅用于用户显式操作路径 (ArkTS 任务栏/窗口点击),
    // 会对已 fullscreen 的目标重新取全屏优先级号; tl_set_fullscreen 等
    // 批处理路径必须保持默认 false。见 ToplevelState::fsPriority 注释
    void RaiseToplevel(uint32_t id, bool userInitiated = false);
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
    uint32_t GetDesktopRootToplevelId() const { return desktopRootToplevelId_; }
    // wl_surface → toplevelId 反查 (PointerExtras 判相对模式的约束 surface
    // 是否桌面 root 自身 — 区分"桌面 shell 启动瞬时藏光标"与"游戏真相对模式")
    // 已删转发 (重构第 6A 步): PointerExtras 装配注入 ToplevelManager 引用直调
    // FindToplevelBySurface (见 pointer_extras.h BindWaylandRefs)
    void SetDesktopRootRecognitionEnabled(bool enabled) { desktopRootMgr_.SetRecognitionEnabled(enabled); }
    /* 首启 wineboot 期间抑制窗口创建事件 (PC 窗口模式): wineboot 的
     * "Setting up Wine" 等待窗不创建独立 OHOS 窗口 — 与 Pad 桌面模式对齐
     * (初始化阶段 desktop root 未出现, 窗口天然不可见)。仅首启 wineboot
     * 生命周期内置位, wineboot 完成后恢复 (wine_launch.cpp)。
     * 抑制状态随事件通道收口于 ToplevelEventBus (重构第 5D 步, 语义不变)。
     * 6A 保留: wine_launch (启动编排入口模块) 唯一调用方, 入口依赖保留委托。 */
    void SetToplevelEventSuppressed(bool on) { toplevelEventBus_.SetSuppressed(on); }
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

    // 交互式窗口移动 (xdg_toplevel.move) — 由 xdg_shell 和 InputManager 调用。
    // 状态查询 (IsMoveGrabActive/GetMoveGrabToplevelId) 已删转发 — 调用方
    // (InputManager) 构造注入 MoveGrabHandler 引用直调 (重构第 6A 步);
    // 动作方法保留 (EndMoveGrab/ProcessMoveGrabMotion 含会话侧 dirty 标记
    // 与事件补发, 属会话状态职责)。
    void StartMoveGrab(uint32_t toplevelId, uint32_t serial);
    void EndMoveGrab();
    bool ProcessMoveGrabMotion(int32_t gx, int32_t gy);

private:
    WaylandServer() = default;
    void EventLoop();
    // 会话首帧 focus 策略 (重构第 5B1 步): 首个 commit 到达时通知 ArkTS
    // active 事件 + 预设 pointer/keyboard focus (Wine 在用户操作前就需要
    // enter)。firstFrame_ 会话级一次性 CAS 判定 (Start/ResetSessionState/
    // ResetFirstFrame 复位), 注入有 Seat 资源安全检查。wl_core FinishCommit
    // 只陈述"首帧 commit 发生"不亲自决策; 回归基线: 决策条件/注入顺序/
    // 参考 (HarmonyBox) 与原内联段逐字 (见 wayland_server.cpp)。
    void TryBeginSessionFirstFrame(uint32_t toplevelId, wl_resource* surfRes);
    // stopAll 主动清空全部 toplevel: SIGKILL 强杀 Wine 后 client 断开事件
    // 未被 dispatch (wl_display_terminate 提前终止事件循环), 依赖断开事件触发
    // 的 OnToplevelDestroyed 不执行 → toplevel 残留 (重启后旧窗口画面共存、
    // 占 zOrder、不响应事件)。此处显式遍历逐个收口并补发 destroyed 给 ArkTS。
    void DestroyAllToplevels();

    // -- surface_commit 分段 (Phase 3B, 实现在 wl_core.cpp) --
    // 协议语义见各函数定义处注释; ShmCommitInfo 在 surface_data.h
    bool HandleNullBufferCommit(SurfaceData* sd, wl_resource* surfRes);
    bool BeginShmAccess(SurfaceData* sd, ShmCommitInfo& fi);
    void ComputeContentArea(SurfaceData* sd, ShmCommitInfo& fi);
    // CommittedSurface 快照产出 (重构第 5A2 步): commit 管线与旧字段同源并行
    // 填充 sd->committed (命名快照), 供消费端切换 (见 committed_surface.h)
    void BuildCommittedSurface(SurfaceData* sd, ShmCommitInfo& fi);

    void UpdateToplevelFrameOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                     ShmCommitInfo& fi, bool& outFirstCommit);
    void CheckDesktopRootOnCommit(SurfaceData* sd, ShmCommitInfo& fi, bool isFirstCommit);
    void UpdateSubsurfaceOnCommit(SurfaceData* sd, wl_resource* surfRes, ShmCommitInfo& fi);
    void UpdateSubsurfaceLayerOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                       uint32_t parentId, ShmCommitInfo& fi);
    void FinishCommit(SurfaceData* sd, wl_resource* surfRes);

    wl_display* display_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // toplevel 状态存储已移入 ToplevelManager (compositor/toplevel_manager.h)
    ToplevelManager toplevelMgr_;

    void MarkDesktopRootDirtyLocked() { desktopRootMgr_.MarkRootDirtyLocked(); }

    StateCb stateCb_;
    // toplevel 事件总线 (重构第 5D 步): 事件名 enum 化 + JSON 构造单点 +
    // NAPI 通道移出 compositor 核心 — 事件通道/抑制门禁/派发日志全部由
    // bus 收口 (bus 零 napi/wine_process 依赖), 本类只留会话侧旁路
    // (desktop_root → MarkDesktopShellProcesses + evt:desktop-ready)。
    // 旧 ToplevelCb/置位标志的宿主成员随收口删除 (见文件头注释)。
    ToplevelEventBus toplevelEventBus_;
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
    // fireEvent_: DesktopRootManager 只发 desktop_root 事件 (PromotePending
    // 路径), 事件名/JSON 与 CheckRootLocked 的 fireDesktopRoot 分支一致 —
    // 收口到 PostToplevelEvent (重构第 5D 步, 事件 enum 化)。
    DesktopRootManager desktopRootMgr_{toplevelMgr_, desktopRootToplevelId_,
                                        pendingDesktopRootToplevelId_,
                                        desktopRootRecognitionEnabled_,
                                        [this](uint32_t id, const char*, const char*) {
                                            PostToplevelEvent(id, ToplevelEventType::DesktopRoot);
                                        }};
    // 帧合成 + zero-copy layer 管理 — 已移入 DesktopCompositor
    DesktopCompositor desktopCompositor_{toplevelMgr_, policy_, desktopRootToplevelId_,
                                          outputW_, outputH_};
    // 输入命中裁决 — 已移入 InputResolver
    InputResolver inputResolver_{toplevelMgr_, desktopCompositor_, desktopRootToplevelId_,
                                  outputW_, outputH_};
    // PC 模式 popup 登记/裁剪/状态管理 — 已移入 PopupManager (重构第 5B2 步;
    // popup 表从 ToplevelManager 迁入, 锁域不变 — tmgr 锁守护, 见 popup_manager.h)
    PopupManager popupMgr_{toplevelMgr_, outputW_, outputH_};
};

#include "compositor/surface_data.h"  // SurfaceData 已提取至独立头文件
