#include "input_manager.h"
#include "seat.h"
#include "pointer_extras.h"
#include "compositor/wayland_server.h"  // 保留: FlushQueue 的 move grab 动作委托 + InputTarget 别名
#include "compositor/input/input_space_mapper.h"  // 坐标变换收口 (4C1): renderer 查找
                                            // fallback 已迁入, 本文件不再认识
                                            // PluginManager (include 已删)
// 6A 装配注入直调 (BindCompositorDeps 注入引用; 见 input_manager.h 注释):
#include "compositor/input/input_resolver.h"   // FindInputTargetAt/SurfaceLocalToDesktop/IsSurfaceAlive(经 injector)
#include "compositor/toplevel/toplevel_manager.h" // GetSurfaceForToplevel/GetToplevelGeometrySnapshot
#include "compositor/toplevel/move_grab.h"        // IsActive/GetToplevelId
#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Input"
#include <hilog/log.h>

// -- Linux evdev button codes --
enum {
    BTN_LEFT   = 0x110,
    BTN_RIGHT  = 0x111,
    BTN_MIDDLE = 0x112,
};

// ClampToContent / ComputeLocalPoint 已收进 compositor/geometry.h
// (重构第 4A 步: 内容区钳制与输入逆映射作为纯函数单点化, 本文件不再持有)。

// (丢帧统计 gDrop* 已随注入层迁至 compositor/input_injector.cpp —
//  全部 fetch_add 点都在 wl_*_send_* 注入函数内, 重构第 4C2 步)

// -- 单例 --
InputManager* InputManager::GetInstance() {
    static InputManager s;
    return &s;
}

// 静止 tap 的最小按压时长: 保证 down/up 落在不同的 GetDeviceState 轮询
// 窗口 (PAL2 按帧轮询 dinput, ~55ms/帧 @18fps — 理论上 ≥1 个轮询帧 (55ms)
// 即可分开 down/up, 取 100ms 再留帧耗时抖动的余量; 见 ACT_RELEASE 脉冲拉伸)
static constexpr uint32_t kMinPressDurationMs = 100;

// -- 辅助: 当前毫秒时间 (原 file-static; 注入层各自独立一份同款) --
static uint32_t NowMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return static_cast<uint32_t>(ms);
}

// ========================================================================
//  生命周期
// ========================================================================

InputManager::InputManager() : injector_(&tracker_) {
    // 四层持成员对象, tracker_ 先于 injector_ 声明 (成员构造顺序), 注入其
    // 引用 — Injector 的焦点/串行号状态写入与编排层读同一 tracker。
}

void InputManager::Initialize(wl_display* display) {
    // 队列+pipe+事件源全迁 InputQueue (原 Initialize 函数体逐字搬移,
    // 含日志与 already-initialized 守卫); flush 回调由本类在事件循环启动前
    // 注入 — 只在 Wayland 线程执行 (与 4C1 warpSink 同模式, 无锁)。
    queue_.Initialize(display, [this] { FlushQueue(); });
}

void InputManager::BindCompositorDeps(ToplevelManager& tmgr, InputResolver& resolver,
                                      MoveGrabHandler& moveGrab, const DisplayPolicy& policy,
                                      const uint32_t& desktopRootToplevelId) {
    // 6A 装配: 见头注释 (生命周期/无锁论证)。injector 的 surface 存活防御
    // (IsSurfaceAlive) 与 surface→toplevel 查询同样走 Subscriber 注入的引用。
    tmgr_ = &tmgr;
    resolver_ = &resolver;
    moveGrab_ = &moveGrab;
    policy_ = &policy;
    desktopRootToplevelId_ = &desktopRootToplevelId;
    injector_.BindResolvers(&resolver, &tmgr);
}

void InputManager::Shutdown() {
    // 队列资源 (pipeSource_/fd/display) 清理迁 InputQueue
    queue_.Shutdown();
    // 清理状态 (原第二段逐字, 顺序: buttons → modifiers → pointer/kbd 焦点)
    tracker_.ResetButtons();
    tracker_.ResetModifiers();
    tracker_.ClearPointerFocus();
    tracker_.ClearKeyboardFocus();

    OH_LOG_INFO(LOG_APP, "[Input] shutdown OK");
}

// ========================================================================
//  坐标转换 (已迁 InputSpaceMapper, 本函数为公开委托 — 重构第 4C1 步)
// ========================================================================

void InputManager::CoordTransform(double px, double py, uint32_t tl,
                                   wl_fixed_t* outX, wl_fixed_t* outY,
                                   FitRect* outLb) {
    // renderer 查找 fallback 链 (tl → root → any) 与 letterbox 逆映射收口在
    // compositor/input_space_mapper.cpp (原函数体逐字搬移, 含 2B 契约化
    // GetInputLetterbox 锚点与抽样日志); 调用线程 (NAPI) 不变。
    InputSpaceMapper::GetInstance()->CoordTransform(px, py, tl, outX, outY, outLb);
}

// ========================================================================
//  指针 warp (wp_pointer_warp_v1) — Wayland 线程调用
// ========================================================================

void InputManager::OnPointerWarp(wl_resource* surface, double sx, double sy) {
    // SetCursorPos 的位置同步。wineserver 光标已在 wine 侧移动到位, host
    // 不需要注入 motion: 绝对模式 (RTS 等) 下一次设备事件自然覆盖, 相对
    // 模式 wine 拒绝 SetCursorPos (wayland_pointer.c:1024) 不会发本请求。
    // 这里只把 move grab 的偏移基准同步到 warp 位置。
    double lx = sx, ly = sy;
    // IsDesktopMode→Policy (重构第 4C1 步): 模式位真策略分支改命名查询 —
    // "desktop 才做 surface 局部→桌面坐标换算" = 输入由 compositor 自路由
    // 的语境 (CompositorRoutesInput; desktop 模式下与 RootCompositing 同值)
    // 6A: 经装配注入的共享 policy 引用直读 (与 WaylandServer::Policy 同值)
    if (policy_->CompositorRoutesInput()) {
        // 6A: 逆映射直呼注入的 InputResolver (原 WaylandServer::SurfaceLocalToDesktop 转发)
        if (!resolver_->SurfaceLocalToDesktop(surface, sx, sy, lx, ly)) {
            OH_LOG_WARN(LOG_APP, "[Input] WARP sync failed: surf=%{public}p not mapped",
                        static_cast<void*>(surface));
            return;
        }
        // 全局指针位置显式语义 (4C1): desktop 分支 = SurfaceLocalToDesktop 后
        // 的桌面逻辑坐标 (Space::Desktop)
        InputSpaceMapper::GetInstance()->UpdateGlobalPtr(
            wl_fixed_from_double(lx), wl_fixed_from_double(ly), GlobalPtrState::Space::Desktop);
    } else {
        // PC 分支 = surface 局部坐标原值 (未经换算, 也不加窗口位置 — 历史语义,
        // 4C1 只重标为 Space::Window 不修正, 见 input_space_mapper.h 注释)
        InputSpaceMapper::GetInstance()->UpdateGlobalPtr(
            wl_fixed_from_double(lx), wl_fixed_from_double(ly), GlobalPtrState::Space::Window);
    }
    static uint32_t sWarpN = 0;
    if (++sWarpN % 120 == 1)
        OH_LOG_INFO(LOG_APP, "[Input] WARP pos=(%{public}.1f,%{public}.1f) desktop=%{public}d n=%{public}u",
                    lx, ly, policy_->CompositorRoutesInput() ? 1 : 0, sWarpN);
}

// ========================================================================
//  Focus 查询与状态复位
// ========================================================================

bool InputManager::NeedsPointerEnter() const {
    auto* seat = Seat::GetInstance();
    // 需要 enter 当: 有 pointer resource 且没有已聚焦的 toplevel
    // (组合判定收在 StateTracker::PointerNeedsEnter — seat 状态作参数传入)
    return tracker_.PointerNeedsEnter(seat->HasPointerResource());
}

void InputManager::ResetPointerEnter() {
    tracker_.ClearPointerFocus();
    OH_LOG_INFO(LOG_APP, "[Input] ResetPointerEnter OK");
}

void InputManager::ResetKeyboardEnter() {
    tracker_.ClearKeyboardFocus();
    OH_LOG_INFO(LOG_APP, "[Input] ResetKeyboardEnter OK");
}

void InputManager::ResetSessionState() {
    // Wine 会话结束后的全量状态复位。残留风险: 焦点指向已销毁的 toplevel;
    // 按下/修饰键残留会让新会话卡键 (会话结束时 Ctrl 按着, 新会话所有按键
    // 都带 Ctrl); 指针位置/相对增量基线污染新会话首次操作。只清状态不发
    // 事件 — client 已断开, send 到已销毁 surface 会触发协议错误。
    // (顺序与旧实现逐字: pointer → keyboard → buttons → modifiers →
    //  global ptr (mapper) → 相对基线 → press 时刻 → 可见性表)
    ResetPointerEnter();
    ResetKeyboardEnter();
    tracker_.ResetButtons();
    tracker_.ResetModifiers();
    // 全局指针位置已收进 InputSpaceMapper (4C1): 原 "字段=0" 改复位调用,
    // 值等价 (标签回默认 Desktop); 位置仍在 modifiers 之后、相对增量基准之前,
    // 与旧实现清零顺序一致。
    InputSpaceMapper::GetInstance()->ResetGlobalPtr();
    tracker_.ResetLastLocal();
    tracker_.ResetLastPressMs();
    tracker_.ClearVisible();
    OH_LOG_INFO(LOG_APP, "[Input] session state reset (focus/buttons/modifiers/position/visible)");
}

void InputManager::OnSurfaceDestroyed(wl_resource* surface) {
    // surface 已被 Wine 销毁, 如果仍持有引用并在后续 Inject*Leave 中使用,
    // 会导致 Wayland 协议错误 "invalid object" → Wine 断开连接
    if (tracker_.PointerFocusedSurfaceIs(surface)) {
        OH_LOG_INFO(LOG_APP, "[Input] OnSurfaceDestroyed: clearing pointer focus (surface=%{public}p was tl=%{public}u)",
                    surface, tracker_.PointerFocusedToplevel());
        tracker_.ClearPointerFocus();
    }
    if (tracker_.KeyboardFocusedSurface() == surface) {
        OH_LOG_INFO(LOG_APP, "[Input] OnSurfaceDestroyed: clearing keyboard focus (surface=%{public}p was tl=%{public}u)",
                    surface, tracker_.KeyboardFocusedToplevel());
        tracker_.ClearKeyboardFocus();
    }
}

// ========================================================================
//  NAPI 入口 (JS 线程)
// ========================================================================

void InputManager::SetToplevelVisible(uint32_t tl, bool visible) {
    tracker_.SetToplevelVisible(tl, visible);
    OH_LOG_INFO(LOG_APP, "[Input] SetToplevelVisible tl=%{public}u visible=%{public}s", tl, visible ? "true" : "false");
}

void InputManager::SendPointerEvent(uint32_t tl, int action, double px, double py, int button,
                                    double rawDx, double rawDy, bool fromMouse) {
    // 窗口不可见时抑制输入
    if (tracker_.IsInputSuppressed(tl)) {
        // 抽样 120:1: 窗口不可见时 hover 移动也会走到这里 (125Hz 全量会
        // 刷屏); 只保留采样行确认"输入被抑制"这一状态
        static uint32_t sSuppressN = 0;
        if (++sSuppressN % 120 == 1)
            OH_LOG_INFO(LOG_APP, "[Input] SUPPRESS tl=%{public}u action=%{public}d (window invisible)", tl, action);
        return;
    }

    auto* seat = Seat::GetInstance();

    // ArkTS MouseAction: Press=1, Release=2, Move=3
    // 注意: Move=3 不是 2! 旧代码曾误将此值交换导致 MOVE/RELEASE 错位
    const int ACT_PRESS   = 1;
    const int ACT_RELEASE = 2;
    const int ACT_MOVE    = 3;

    // 无 pointer resource 时所有事件跳过
    if (!seat->HasPointerResource()) return;

    // 坐标转换
    wl_fixed_t wx, wy;
    // Desktop 模式: 按桌面坐标解析精确输入目标 (菜单 subsurface 有自己的
    // wl_surface, 必须 enter 它并用层相对坐标 — 经父窗口 surface 的越界
    // 坐标会被 winewayland 的 motion clamp 夹回窗口内, 菜单伸出部分点不中)
    // 6A: 策略/rootId 经装配注入的共享引用直读 (与 WaylandServer getter 同值)
    wl_resource* targetSurf = nullptr;
    if (policy_->CompositorRoutesInput() && tl != *desktopRootToplevelId_) {
        CoordTransform(px, py, *desktopRootToplevelId_, &wx, &wy);
        // 记录最近一次注入的桌面全局指针位置 (grab 建立时算固定偏移用)。
        // 4C1: 收进 InputSpaceMapper, 空间标签显式化为 Desktop (桌面逻辑坐标)
        InputSpaceMapper::GetInstance()->UpdateGlobalPtr(wx, wy, GlobalPtrState::Space::Desktop);
        // move grab 期间 (xdg_toplevel.move): compositor 用桌面全局坐标绝对定位
        // 被拖窗口, motion 必须注入全局坐标。局部坐标往返 (enqueue 时
        // local = logical - st->x, 消费时再 + st->x 还原) 在两个线程间基准
        // 漂移: 消费时刻的 st->x 已变, rx 多出"窗口自身刚移动的量"并叠进
        // 下一帧位移 → 快速拖动时窗口位移逐帧累积放大, 窗口瞬间飞出屏幕
        // 6A: moveGrab 状态查询直呼注入的 MoveGrabHandler (原两行转发)
        if (action == ACT_MOVE && moveGrab_->IsActive() &&
            moveGrab_->GetToplevelId() == tl) {
            queue_.Enqueue(InputQueue::Event::PTR_MOTION, 0, nullptr, wx, wy, 0, 0);
            return;
        }
        double logicalX = wl_fixed_to_double(wx);
        double logicalY = wl_fixed_to_double(wy);
        WaylandServer::InputTarget target;
        if (resolver_->FindInputTargetAt(logicalX, logicalY, target)) {
            // 全屏黑边: 只吞 PRESS (防幻影点击/焦点切换)。MOVE/RELEASE 照常透传 —
            // 越界坐标已在 resolver 内钳到内容区边缘 (宿主侧钳制, 与相对增量
            // 差分同源; 不再依赖 winewayland clamp, 否则相对增量差分会累积
            // 黑边里的幽灵位移); 吞掉 RELEASE 会让 pressedButtons_ 永不清位
            // (按键卡死)
            if (target.swallow && action == ACT_PRESS) return;
            // WineHua modal 拦截: 命中被模态禁用的 owner (blockedModalId > 0):
            // 吞掉 PRESS (禁用窗口不接收点击, Win32 语义), 同时键盘焦点切到
            // 模态框 + raise 其 owner 组 (RaiseToplevel 把 modal 上浮到主人)
            // — 首次点击只激活模态框, 第二次点击落在模态框上正常注入。
            // 只吞 PRESS: MOVE 透传 (hover 无点击语义, 吞掉会让相对模式增量
            // 基线失真), RELEASE 透传 (按下在模态框、拖到 owner 区域松手的
            // 组合若吞 release, pressedButtons_ 永不清位 — 与全屏黑边的
            // swallow 只吞 PRESS 同源)。
            if (target.blockedModalId && action == ACT_PRESS) {
                OH_LOG_INFO(LOG_APP, "[Input] MODAL-BLOCK owner tl=%{public}u press swallowed,"
                            " activate modal #%{public}u", target.toplevelId, target.blockedModalId);
                // 走 WaylandServer::RaiseToplevel (内部持锁, 与 Wayland 线程
                // commit 的 z-order 写互斥; 裸调 tmgr_ = 无锁 push_back 并发
                // 时数组重分配/悬垂指针)。userInitiated=true 与 ArkTS 点击 raise
                // 同语义: 任务栏 PinToTop + 桌面 dirty 保持一致
                WaylandServer::GetInstance()->RaiseToplevel(target.blockedModalId, true);
                if (!tracker_.KeyboardEntered() ||
                    tracker_.KeyboardFocusedToplevel() != target.blockedModalId) {
                    wl_resource* kbdSurf = tmgr_->GetSurfaceForToplevel(target.blockedModalId);
                    if (kbdSurf) {
                        if (tracker_.KeyboardEntered())
                            queue_.Enqueue(InputQueue::Event::KBD_LEAVE, 0, nullptr, 0, 0, 0, 0);
                        tracker_.SetKeyboardFocus(target.blockedModalId, kbdSurf);
                        queue_.Enqueue(InputQueue::Event::KBD_ENTER, target.blockedModalId,
                                       kbdSurf, 0, 0, 0, 0);
                        EnqueueModifiers();
                    }
                }
                return;
            }
            tl = target.toplevelId;
            targetSurf = target.surface;
            // 裁决闭环 (重构第 4A 步): 桌面坐标 → surface 局部坐标的逆映射与
            // 内容区钳制已由 InputResolver 收内 (终态 localX/localY), 调用方
            // 只做 wl_fixed 注入 — 不再手写 (logical-origin)/scale + ClampToContent
            wx = wl_fixed_from_double(target.localX);
            wy = wl_fixed_from_double(target.localY);
            // 系统性链路日志 (断点 2): 目标解析结果 — 手指桌面坐标命中哪个窗口/
            // 区域, 全屏时 origin+scale 即 fit 几何 (内容/黑边), local 是最终
            // 注入的 wine 坐标。PRESS/RELEASE 全量 (点按诊断核心), MOVE 高频
            // 抽样 120:1 (拖动只看轨迹趋势, 防刷爆 hilog — 测试需全量时改这里)
            static uint32_t sTargetLogN = 0;
            if (action != ACT_MOVE || ++sTargetLogN % 120 == 0)
                OH_LOG_INFO(LOG_APP, "[Input] TARGET a=%{public}d px=(%{public}.0f,%{public}.0f) → tl=%{public}u"
                            " surf=%{public}p swallow=%{public}d origin=(%{public}.1f,%{public}.1f) scale=%{public}.2f"
                            " → local=(%{public}.1f,%{public}.1f)",
                            action, logicalX, logicalY, target.toplevelId,
                            static_cast<void*>(target.surface),
                            target.swallow ? 1 : 0, target.originX, target.originY,
                            target.scale, target.localX, target.localY);
        } else {
            // 目标 surface 不可用: 退回旧路径 (父窗口相对坐标)
            const auto tlGeo = tmgr_->GetToplevelGeometrySnapshot(tl);
            wx = wl_fixed_from_double(logicalX - tlGeo.x);
            wy = wl_fixed_from_double(logicalY - tlGeo.y);
            // 目标 surface 不可用是异常路径 (正常应命中 root), WARN 全量
            OH_LOG_WARN(LOG_APP, "[Input] TARGET-FALLBACK a=%{public}d px=(%{public}.0f,%{public}.0f) tl=%{public}u"
                        " → local=(%{public}.1f,%{public}.1f) (no surf)",
                        action, logicalX, logicalY, tl,
                        logicalX - tlGeo.x, logicalY - tlGeo.y);
        }
    } else {
        FitRect lb{};
        CoordTransform(px, py, tl, &wx, &wy, &lb);
        // 钳到内容区 (全屏 letterbox 黑边 / 拖出窗口边缘的越界坐标):
        // 与可见光标位置对齐, 防相对增量差分累积幽灵位移 (见 ClampToContent)
        if (lb.srcW > 0 && lb.srcH > 0) {
            wx = wl_fixed_from_double(ClampToContent(wl_fixed_to_double(wx), lb.srcW));
            wy = wl_fixed_from_double(ClampToContent(wl_fixed_to_double(wy), lb.srcH));
        }
        // PC 空间全局指针位置 = 窗口局部坐标 + 窗口位置 (grab 偏移基准)。
        // 4C1: 显式语义为 Window 空间 (窗口局部+窗口位置还原值)
        const auto tlGeo = tmgr_->GetToplevelGeometrySnapshot(tl);
        InputSpaceMapper::GetInstance()->UpdateGlobalPtr(
            wl_fixed_from_double(wl_fixed_to_double(wx) + tlGeo.x),
            wl_fixed_from_double(wl_fixed_to_double(wy) + tlGeo.y),
            GlobalPtrState::Space::Window);
        // move grab 降级路径 (PC 模式 startMoving 失败时): wx 是窗口局部坐标,
        // 补上窗口位置还原为绝对坐标, 供 compositor 绝对定位 (不在此做
        // 局部→全局往返, 消费侧不再二次读 st->x, 避免双线程基准漂移)
        if (action == ACT_MOVE && moveGrab_->IsActive() &&
            moveGrab_->GetToplevelId() == tl) {
            queue_.Enqueue(InputQueue::Event::PTR_MOTION, 0, nullptr,
                    wl_fixed_from_double(wl_fixed_to_double(wx) + tlGeo.x),
                    wl_fixed_from_double(wl_fixed_to_double(wy) + tlGeo.y),
                    0, 0);
            return;
        }
        // PC 模式: wx/wy 即窗口局部坐标, 无需额外变换
    }

    // 相对指针增量 (zwp_relative_pointer_v1): wine 相对模式 (隐藏光标 + 约束,
    // wayland_pointer.c needs_relative) 丢弃绝对 motion, 光标位置 = 基线 +
    // 增量累积。host 不做模式判断 — 绝对 motion 照常注入 (相对模式下被 wine
    // 丢弃), 有 relative 对象就入队 REL_MOTION。对象存在 ⇔ wine 判定当前为
    // 相对模式。
    // 增量来源: 优先用鸿蒙 MouseEvent.rawDelta (鼠标硬件原始增量) — 光标
    // 被 ClampToContent 钳在屏幕/窗口边缘后, 绝对坐标差分恒为 0, 只有
    // rawDelta 还在真实上报; 若仍用差分, dinput 视角游戏 (FPS) 鼠标顶到
    // 边缘即卡死。rawDelta 缺失时 (触屏合成 mouse 等) 回退绝对差分。
    // 基准在 MOVE 与「PRESS 的 enter 定位」时更新 — 两者都真实改变 wine
    // 光标位置 (enter 是相对模式下唯一被消费的绝对坐标); 其余 PRESS/RELEASE
    // 不发增量也不移动基准, 避免按下瞬间坐标跳变污染后续增量/标定样本。
    if (action == ACT_MOVE) {
        const double localX = wl_fixed_to_double(wx);
        const double localY = wl_fixed_to_double(wy);
        const double diffDx = tracker_.HasLastLocal() ? (localX - tracker_.LastLocalX()) : 0.0;
        const double diffDy = tracker_.HasLastLocal() ? (localY - tracker_.LastLocalY()) : 0.0;
        if (PointerExtras::GetInstance()->HasRelativePointer()) {
            double dx = 0.0, dy = 0.0;
            if (rawDx != 0.0 || rawDy != 0.0) {
                // 相对模式视角: rawDelta 已由 ArkTS 按设备类型缩放 (鼠标
                // 2.5 / 触控板 0.75, 见 InputDeviceMapper.ets), C++ 直接使用。
                // 单事件位移钳制: 防异常巨型跳变 (125Hz 合法单事件远小于此)
                dx = std::clamp(rawDx, -512.0, 512.0);
                dy = std::clamp(rawDy, -512.0, 512.0);
            } else {
                dx = diffDx;
                dy = diffDy;
            }
            if (dx != 0.0 || dy != 0.0) {
                queue_.Enqueue(InputQueue::Event::REL_MOTION, 0, nullptr,
                        wl_fixed_from_double(dx), wl_fixed_from_double(dy), 0, 0);
                // 系统性链路日志 (断点 4): 相对模式增量 — 相对模式下绝对
                // motion 被 wine 丢弃, 增量是光标唯一移动来源; 高频抽样 120:1
                // (与 SendRelativeMotion 断点 5 配对; 测试需全量时改这里)
                static uint32_t sRelLogN = 0;
                if (++sRelLogN % 120 == 0)
                    OH_LOG_INFO(LOG_APP, "[Input] REL d=(%{public}.1f,%{public}.1f) raw=(%{public}.1f,%{public}.1f)"
                                " base=(%{public}.1f,%{public}.1f)",
                                dx, dy, rawDx, rawDy, tracker_.LastLocalX(), tracker_.LastLocalY());
            }
        }
        tracker_.UpdateLastLocal(localX, localY);
    }


    // MOVE 是高频路径 (hover 移动 ~125Hz), 全量日志会刷爆 hilog → 抽样 120:1;
    // PRESS/RELEASE 低频且诊断价值高, 保持全量
    if (action != ACT_MOVE) {
        OH_LOG_INFO(LOG_APP, "[Input] PTR action=%{public}d tl=%{public}u btn=0x%{public}x px=(%{public}.0f,%{public}.0f)"
                    " wine=(%{public}.0f,%{public}.0f) ptrRes=%{public}d needsEnter=%{public}d pressedBits=0x%{public}x",
                    action, tl, button, px, py,
                    wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                    seat->HasPointerResource(), NeedsPointerEnter(), tracker_.PressedButtons());
    } else {
        static uint32_t sMoveLogN = 0;
        if (++sMoveLogN % 120 == 0)
            OH_LOG_INFO(LOG_APP, "[Input] PTR MOVE tl=%{public}u px=(%{public}.0f,%{public}.0f)"
                        " wine=(%{public}.0f,%{public}.0f) focusedTl=%{public}u n=%{public}u",
                        tl, px, py, wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                        tracker_.PointerFocusedToplevel(), sMoveLogN);
    }

    switch (action) {
        case ACT_PRESS: {
            // 每次点击都重发 enter 定位: 触摸屏是绝对定位设备, 点按 =
            // "光标跳到手指位置再点击" — wine 的 pointer_handle_enter 更新
            // 光标绝对位置。方向 A (wayland_pointer.c 静默校准): 相对模式下
            // enter 走 NtUserSetCursorPos 静默路径, 只更新 wineserver 光标
            // 位置不产生硬件输入 — 读绝对位置的老游戏 (红警2/模拟邻居) 点按
            // 定位正常, 真 dinput 视角游戏 (PAL2) 增量差分通道不受污染
            // (曾需 tapPositioning 开关二选一, 已删除: 两类游戏现在同时正常)。
            // 桌面模式需 surface 级比较 (菜单层与父窗口同 toplevelId), 其余
            // 模式保持 toplevel 级比较 (一窗一 surface, 语义等价)。
            wl_resource* pressTargetSurf =
                targetSurf ? targetSurf : tmgr_->GetSurfaceForToplevel(tl);
            // 相对模式 + 物理鼠标: 跳过 enter/motion 重定位, 只投递按键。
            // LockCursor 冻结系统光标后, 每次点击的坐标恒为冻结点, 而 wine
            // 相对模式把 enter 当静默 SetCursorPos — 游戏自绘光标走绝对
            // GetCursorPos (war3) 每次点击都瞬移到固定位置 (实测: 冻结点
            // 633,392 → local 356.7,256.0 恒定)。触屏 tap 仍是绝对定位设备,
            // 保留 enter (PAL2 点菜单依赖); 指针尚未聚焦该 surface 时也必须
            // enter, 否则 button 无焦点投递
            const bool skipEnter = fromMouse
                && PointerExtras::GetInstance()->HasRelativePointer()
                && pressTargetSurf != nullptr
                && tracker_.PointerFocusedSurfaceIs(pressTargetSurf);
            // 系统性链路日志 (断点 3): relMode/skipEnter 仅诊断 (相对模式下
            // enter 走静默校准, 行为与绝对模式一致; skipEnter=1 时 enter 被跳过)
            OH_LOG_INFO(LOG_APP, "[Input] PRESS-ENTER tl=%{public}u surf=%{public}p"
                        " relMode=%{public}d skip=%{public}d focused=%{public}p",
                        tl, static_cast<void*>(pressTargetSurf),
                        PointerExtras::GetInstance()->HasRelativePointer() ? 1 : 0,
                        skipEnter ? 1 : 0,
                        static_cast<void*>(tracker_.PointerFocusedSurface()));
            if (pressTargetSurf && !skipEnter) {
                // enter/leave 共同动作 (needLeave 双判据 + LEAVE/ENTER 入队序)
                // 收敛于 SubmitEnterLeave — 语义与旧内联段逐字一致
                SubmitEnterLeave(tl, targetSurf, pressTargetSurf, wx, wy);
                // enter 定位真实改变 wine 光标位置 — 保持增量基线与 wine
                // 光标一致, 防后续拖动漂移
                tracker_.UpdateLastLocal(wl_fixed_to_double(wx), wl_fixed_to_double(wy));
            }
            if (!skipEnter)
                queue_.Enqueue(InputQueue::Event::PTR_MOTION, 0, nullptr, wx, wy, 0, 0);
            if (button) {
                unsigned bit = tracker_.ButtonToBit(button);
                if (bit < 32) {
                    tracker_.OnButtonPress(button);
                    OH_LOG_INFO(LOG_APP, "[Input] BTN_PRESS btn=0x%{public}x bit=%{public}u pressedBits=0x%{public}x",
                                button, bit, tracker_.PressedButtons());
                }
                tracker_.SetLastPressMs(NowMs());  // 脉冲拉伸计时基准 (见 ACT_RELEASE)
                queue_.Enqueue(InputQueue::Event::PTR_BUTTON, 0, nullptr, 0, 0, button,
                               WL_POINTER_BUTTON_STATE_PRESSED);
            }

            //  键盘焦点跟随点击 (P0-1 + P0-3)
            // winewayland.drv: keyboard_enter → WM_WAYLAND_SET_FOREGROUND
            // → NtUserSetForegroundWindowInternal → Wine 前台窗口切换
            if (!tracker_.KeyboardEntered() || tracker_.KeyboardFocusedToplevel() != tl) {
                wl_resource* kbdSurf = tmgr_->GetSurfaceForToplevel(tl);
                if (kbdSurf) {
                    if (tracker_.KeyboardEntered() && tracker_.KeyboardFocusedToplevel() != tl)
                        queue_.Enqueue(InputQueue::Event::KBD_LEAVE, 0, nullptr, 0, 0, 0, 0);
                    // 立即设置状态, 避免 NAPI 线程在 flush 前又发一次 enter
                    tracker_.SetKeyboardFocus(tl, kbdSurf);
                    queue_.Enqueue(InputQueue::Event::KBD_ENTER, tl, kbdSurf, 0, 0, 0, 0);
                    EnqueueModifiers();
                    OH_LOG_INFO(LOG_APP, "[Input] PTR PRESS + KBD ENTER tl=%{public}u (focus follows click)", tl);
                }
            }
            break;
        }
        case ACT_RELEASE: {
            // ArkTS RELEASE 的 button 字段始终为 0x0
            // 从 pressedButtons_ bitmask 中查找被按下的按钮并释放
            // (查找/清除逻辑收在 StateTracker::OnButtonRelease — 语义逐字)
            uint32_t releaseBtn = tracker_.OnButtonRelease(button);
            OH_LOG_INFO(LOG_APP, "[Input] BTN_RELEASE btn=0x%{public}x→0x%{public}x pressedBits=0x%{public}x",
                        button, releaseBtn, tracker_.PressedButtons());
            // 脉冲拉伸: 按下-抬起间隔 <kMinPressDurationMs 的点击, 抬手延迟
            // 补足再发。根因: ArkTS 触控手势状态机把静止 tap 合成为
            // Press+Release 同刻脉冲 (DesktopWindow.ets onTouch 的"等 Up 再
            // 补发"手势语义); down/up 挤在同一次 GetDeviceState 轮询 (~8ms)
            // 内相互净零, PAL2 类按帧轮询 dinput 的游戏永远看不见这次点击
            // (探针实证: smoke/dinput_click_probe.c; 真机 Windows / winlator
            // 的真实时序输入无此问题)。物理鼠标自 ArkTS .onMouse 直通修复后
            // 为真实时序, 本延迟主要对残余的触屏 tap 脉冲生效。
            // 延迟用短生命周期线程, 不阻塞 ArkTS 输入线程; 极限快速连点
            // (<100ms 间隔) 事件可能乱序, 对老游戏可接受。
            const uint32_t quick = NowMs() - tracker_.LastPressMs();
            if (releaseBtn && quick < kMinPressDurationMs) {
                const uint32_t delayMs = kMinPressDurationMs - quick;
                // 可观测性: 触屏 tap 之外的使用 (如物理鼠标) 不应触发本延迟;
                // 触发 = 某条输入路径在产塌缩脉冲, 是 bug 信号而非正常事件
                OH_LOG_INFO(LOG_APP, "[Input] STRETCH delay release btn=0x%{public}x quick=%{public}ums delay=%{public}ums",
                            releaseBtn, quick, delayMs);
                std::thread([this, delayMs, releaseBtn] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                    queue_.Enqueue(InputQueue::Event::PTR_BUTTON, 0, nullptr, 0, 0, releaseBtn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
                }).detach();
            } else if (releaseBtn) {
                queue_.Enqueue(InputQueue::Event::PTR_BUTTON, 0, nullptr, 0, 0, releaseBtn,
                               WL_POINTER_BUTTON_STATE_RELEASED);
            }
            break;
        }
        case ACT_MOVE: {
            // 高频路径不打全量日志 (入口已有 120:1 抽样); MOVE-ENTER 是低频
            // 焦点切换事件, 保留全量日志
            // desktop: surface 级焦点判定 — 鼠标从窗口移入菜单层 (同 toplevelId,
            // 不同 surface) 时必须重新 enter, 否则 motion 继续发给窗口 surface
            const bool needEnter = targetSurf
                ? (NeedsPointerEnter() || tracker_.PointerFocusedSurface() != targetSurf)
                : (NeedsPointerEnter() || tracker_.PointerFocusedToplevel() != tl);
            if (needEnter) {
                wl_resource* surf = targetSurf ? targetSurf : tmgr_->GetSurfaceForToplevel(tl);
                OH_LOG_INFO(LOG_APP, "[Input] MOVE-ENTER try surf=%{public}p for tl=%{public}u", surf, tl);
                if (surf) {
                    SubmitEnterLeave(tl, targetSurf, surf, wx, wy);
                    OH_LOG_INFO(LOG_APP, "[Input] MOVE-ENTER enqueued OK");
                }
            }
            queue_.Enqueue(InputQueue::Event::PTR_MOTION, 0, nullptr, wx, wy, 0, 0);
            break;
        }
        default:
            break;
    }
}

void InputManager::SendKeyEvent(uint32_t tl, int evdevCode, bool pressed) {
    // 窗口不可见时抑制输入
    if (tracker_.IsInputSuppressed(tl)) {
        // 抽样 120:1 (密钥重复按压/自动重复会出现高频, 与指针同一策略)
        static uint32_t sSuppressN = 0;
        if (++sSuppressN % 120 == 1)
            OH_LOG_INFO(LOG_APP, "[Input] SUPPRESS tl=%{public}u evdev=%{public}d (window invisible)", tl, evdevCode);
        return;
    }

    auto* seat = Seat::GetInstance();

    OH_LOG_INFO(LOG_APP, "[Input] KEY tl=%{public}u evdev=%{public}d pressed=%{public}d"
                " kbdRes=%{public}d kbdEntered=%{public}d",
                tl, evdevCode, pressed,
                seat->GetKeyboardResource() ? 1 : 0,
                tracker_.KeyboardEntered());

    // 键盘 enter 管理: 立即设置状态防止重复 enter (参考旧代码)
    // 桌面模式: 键盘事件永远发到 root, 不应覆盖点击建立的子窗口焦点
    // 6A: 策略/surface 查询经装配注入引用直调 (同值)
    if (pressed && !policy_->CompositorRoutesInput()
        && (!tracker_.KeyboardEntered() || tracker_.KeyboardFocusedToplevel() != tl)) {
        wl_resource* surf = tmgr_->GetSurfaceForToplevel(tl);
        if (surf) {
            if (tracker_.KeyboardEntered() && tracker_.KeyboardFocusedToplevel() != tl) {
                queue_.Enqueue(InputQueue::Event::KBD_LEAVE, 0, nullptr, 0, 0, 0, 0);
            }
            // 立即设置状态, 避免 NAPI 线程在 flush 前又发一次 enter
            tracker_.SetKeyboardFocus(tl, surf);
            queue_.Enqueue(InputQueue::Event::KBD_ENTER, tl, surf, 0, 0, 0, 0);
            // 发送初始 modifier 状态
            EnqueueModifiers();
        }
    }

    // 追踪 modifier 状态 → 同步到 Wine
    if (tracker_.IsModifierKey(evdevCode)) {
        tracker_.UpdateModifiers(evdevCode, pressed);
        EnqueueModifiers();  // 每次修饰键变化都同步, Wine 需要最新的 modifier state
    }

    // 入队 key 事件
    uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    queue_.Enqueue(InputQueue::Event::KBD_KEY, 0, nullptr, 0, 0, evdevCode, state);
}

void InputManager::EnqueueModifiers() {
    // 修饰键状态快照入队 (原 InputManager::EnqueueModifiers 逐字 — 事件携带
    // 四个当前值, 与出队时点无关; 值在 NAPI 线程取, 与旧读端一致)
    queue_.EnqueueModifiers(tracker_.ModifiersDepressed(), tracker_.ModifiersLatched(),
                            tracker_.ModifiersLocked(), tracker_.ModifiersGroup());
}

void InputManager::SendScrollEvent(uint32_t tl, int axis, double value, int scrollStep,
                                    double px, double py) {
    // 窗口不可见时抑制输入 (缺段修复: scroll 此前绕过可见性检查, 与
    // SendPointerEvent/SendKeyEvent 对齐 — 最小化窗口/不可见窗口的滚动不再注入)
    if (tracker_.IsInputSuppressed(tl)) {
        // 抽样 120:1: 连续滚动会有高频重复, 与指针/键盘同一策略
        static uint32_t sSuppressN = 0;
        if (++sSuppressN % 120 == 1)
            OH_LOG_INFO(LOG_APP, "[Input] SUPPRESS tl=%{public}u axis=%{public}s (window invisible)",
                        tl, axis == 0 ? "VERT" : "HORIZ");
        return;
    }

    auto* seat = Seat::GetInstance();
    if (!seat->HasPointerResource()) return;

    // 坐标转换 — 缺段修复: 与 SendPointerEvent 同构 — 桌面模式走 root +
    // FindInputTargetAt (4A 终态 localX/localY = 逆映射 + 内容区钳制收内),
    // PC 模式保留 CoordTransform(tl) 并补 ClampToContent (此前是全文件唯一
    // 不钳制的坐标路径: 黑边/越界坐标直接进 enter 会污染 wine 光标位置)。
    // 修复前只用 CoordTransform(px, py, tl) — 桌面模式 tl 是 ArkTS
    // findToplevelAt 的窗口 id, 查不到 renderer 走 GetAnyRenderer 兜底得到
    // 的是**桌面逻辑坐标**, 却以窗口局部坐标语义进 enter → wine 光标被
    // 设置到偏移窗口原点/未经 fit 缩放的位置。
    wl_fixed_t wx, wy;
    wl_resource* targetSurf = nullptr;
    // 6A: 策略/rootId/moveGrab/裁决经装配注入引用直调 (与旧 WaylandServer 转发同值)
    if (policy_->CompositorRoutesInput() && tl != *desktopRootToplevelId_) {
        CoordTransform(px, py, *desktopRootToplevelId_, &wx, &wy);
        // 缺段修复: 维护最近一次全局指针位置 (grab 偏移基准, 与
        // SendPointerEvent 桌面分支同一语义 — scroll 位置即指针位置);
        // 4C1 收进 InputSpaceMapper, 空间标签 Desktop (桌面逻辑坐标)
        InputSpaceMapper::GetInstance()->UpdateGlobalPtr(wx, wy, GlobalPtrState::Space::Desktop);
        // move grab 期间: 交互式拖拽由 compositor 接管, 不注入 scroll —
        // 拖动窗口标题栏时滚轮不应滚动窗口内容 (axis 无位置属性, 不存在
        // 绝对值定位的等价事件, 拖拽中直接丢弃)
        if (moveGrab_->IsActive() && moveGrab_->GetToplevelId() == tl) {
            OH_LOG_INFO(LOG_APP, "[Input] SCROLL-DROP tl=%{public}u (move grab active)", tl);
            return;
        }
        double logicalX = wl_fixed_to_double(wx);
        double logicalY = wl_fixed_to_double(wy);
        WaylandServer::InputTarget target;
        if (resolver_->FindInputTargetAt(logicalX, logicalY, target)) {
            tl = target.toplevelId;
            targetSurf = target.surface;
            // 终态: resolver 锁内已算好 surface 局部坐标 + 内容区钳制
            // (桌面坐标→局部逆映射由 4A ComputeLocalPoint 单点化, 不手写)
            wx = wl_fixed_from_double(target.localX);
            wy = wl_fixed_from_double(target.localY);
            OH_LOG_INFO(LOG_APP, "[Input] SCROLL-TARGET tl=%{public}u surf=%{public}p"
                        " origin=(%{public}.1f,%{public}.1f) scale=%{public}.2f"
                        " swallow=%{public}d → local=(%{public}.1f,%{public}.1f)",
                        tl, static_cast<void*>(target.surface),
                        target.originX, target.originY, target.scale,
                        target.swallow ? 1 : 0, target.localX, target.localY);
        } else {
            // 目标 surface 不可用: 退回旧路径 (父窗口相对坐标), 同 SendPointerEvent
            const auto tlGeo = tmgr_->GetToplevelGeometrySnapshot(tl);
            wx = wl_fixed_from_double(logicalX - tlGeo.x);
            wy = wl_fixed_from_double(logicalY - tlGeo.y);
            OH_LOG_WARN(LOG_APP, "[Input] SCROLL-FALLBACK tl=%{public}u → local=(%{public}.1f,%{public}.1f) (no surf)",
                        tl, logicalX - tlGeo.x, logicalY - tlGeo.y);
        }
    } else {
        FitRect lb{};
        CoordTransform(px, py, tl, &wx, &wy, &lb);
        // 钳到内容区 (与 SendPointerEvent PC 分支同款): 全屏 letterbox 黑边/
        // 拖出窗口边缘的越界滚动坐标钳回内容区, 防 enter 把 wine 光标放到
        // 屏幕外的无效位置
        if (lb.srcW > 0 && lb.srcH > 0) {
            wx = wl_fixed_from_double(ClampToContent(wl_fixed_to_double(wx), lb.srcW));
            wy = wl_fixed_from_double(ClampToContent(wl_fixed_to_double(wy), lb.srcH));
        }
        // 缺段修复: PC 空间全局指针位置 = 窗口局部 + 窗口位置 (grab 偏移基准,
        // 与 SendPointerEvent PC 分支同款); 4C1 显式语义 Window 空间
        const auto tlGeo = tmgr_->GetToplevelGeometrySnapshot(tl);
        InputSpaceMapper::GetInstance()->UpdateGlobalPtr(
            wl_fixed_from_double(wl_fixed_to_double(wx) + tlGeo.x),
            wl_fixed_from_double(wl_fixed_to_double(wy) + tlGeo.y),
            GlobalPtrState::Space::Window);
        if (moveGrab_->IsActive() && moveGrab_->GetToplevelId() == tl) {
            OH_LOG_INFO(LOG_APP, "[Input] SCROLL-DROP tl=%{public}u (move grab active)", tl);
            return;
        }
    }

    // Wayland axis value 用 wl_fixed_t (256 精度)
    // HarmonyOS AxisEvent 的 value 是浮点数, 每个 notch 通常 ±1.0
    wl_fixed_t val = wl_fixed_from_double(value);

    OH_LOG_INFO(LOG_APP, "[Input] SCROLL tl=%{public}u axis=%{public}s val=%{public}.1f step=%{public}d"
                " px=(%{public}.0f,%{public}.0f) wine=(%{public}.1f,%{public}.1f) ptrRes=%{public}d",
                tl, axis == 0 ? "VERT" : "HORIZ", value, scrollStep, px, py,
                wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                seat->HasPointerResource());

    // 确保指针已 enter — 缺段修复: 判定升级到与 ACT_MOVE 同纬 (surface 级)。
    // 仅比较 toplevelId 时, 桌面模式滚动落在菜单 subsurface (与父窗口同
    // toplevelId) 上不会重新 enter, axis 继续喂给父窗口; 且 enter 的 surface
    // 必须是被命中的层自己的 surface (菜单伸出父窗口边界, 用父窗口 surface
    // 的越界局部坐标会被 winewayland 的 motion clamp 夹回窗口内 — 与
    // ACT_MOVE 同一需求)。
    if (targetSurf
        ? (NeedsPointerEnter() || tracker_.PointerFocusedSurface() != targetSurf)
        : (NeedsPointerEnter() || tracker_.PointerFocusedToplevel() != tl)) {
        wl_resource* surf = targetSurf ? targetSurf : tmgr_->GetSurfaceForToplevel(tl);
        OH_LOG_INFO(LOG_APP, "[Input] SCROLL-ENTER try surf=%{public}p for tl=%{public}u", surf, tl);
        if (surf) {
            SubmitEnterLeave(tl, targetSurf, surf, wx, wy);
            OH_LOG_INFO(LOG_APP, "[Input] SCROLL-ENTER enqueued OK");
        }
    }

    // 入队 axis 事件 (原手写 push 收在 InputQueue::EnqueueAxis — 锁边界/pipe
    // 唤醒与原实现逐字; tl 诊断字段语义见 input_queue.h)
    queue_.EnqueueAxis(axis, val, tl);
}

// ========================================================================
//  enter/leave 收敛 helper (三变体共同动作 — 见头注释)
// ========================================================================

void InputManager::SubmitEnterLeave(uint32_t tl, wl_resource* targetSurf,
                                    wl_resource* surf, wl_fixed_t x, wl_fixed_t y) {
    // needLeave 双判据 (与旧 ACT_PRESS/ACT_MOVE/SCROLL-ENTER 三处内联逐字):
    // targetSurf 非空 = 桌面级 — 已有聚焦 surface 且 ≠ 新目标 → leave;
    // 为空 = toplevel 级 — 已有聚焦 toplevel 且 ≠ tl → leave。
    // (注意: surf 由调用者选定 — 桌面级必须用命中的层自己 surface, 菜单
    //  伸出父窗口边界时经父窗口 surface 的越界坐标会被 winewayland clamp)
    wl_resource* focused = tracker_.PointerFocusedSurface();
    const bool needLeave = targetSurf
        ? (focused != nullptr && focused != surf)
        : (tracker_.PointerFocusedToplevel() != 0 && tracker_.PointerFocusedToplevel() != tl);
    if (needLeave)
        queue_.Enqueue(InputQueue::Event::PTR_LEAVE, 0, nullptr, 0, 0, 0, 0);
    queue_.Enqueue(InputQueue::Event::PTR_ENTER, tl, surf, x, y, 0, 0);
}

// ========================================================================
//  事件队列 flush (Wayland 线程 — pipe 回调 → poll 去重 → dispatch → flush)
// ========================================================================

void InputManager::FlushQueue() {
    // InputQueue::Poll = 锁内 swap 取批 + 去重 (原 FlushQueue 前半逐字,
    // dedup 日志在队列层 — 去重属队列批量语义, 与注入无关, 见 input_queue.h)
    auto batch = queue_.Poll();
    if (batch.empty()) return;

    for (auto& ev : batch) {
        switch (ev.type) {
            case InputQueue::Event::PTR_ENTER:   injector_.InjectPointerEnter(ev.tl, ev.surface, ev.x, ev.y); break;
            case InputQueue::Event::PTR_LEAVE:   injector_.InjectPointerLeave(); break;
            case InputQueue::Event::PTR_MOTION: {
                //  Wayland 标准: xdg_toplevel.move 期间 compositor 接管 motion,
                // 不转发给 Wine (协议规定 surface loses device focus)
                if (WaylandServer::GetInstance()->ProcessMoveGrabMotion(ev.x, ev.y))
                    break;
                injector_.InjectPointerMotion(ev.x, ev.y); break;
            }
            case InputQueue::Event::PTR_BUTTON:
                //  交互式移动结束: Release 时结束 grab 并转发给 Wine
                if (ev.state == WL_POINTER_BUTTON_STATE_RELEASED)
                    WaylandServer::GetInstance()->EndMoveGrab();
                injector_.InjectPointerButton(ev.btn_or_key, ev.state); break;
            case InputQueue::Event::REL_MOTION:  injector_.InjectRelativeMotion(ev.x, ev.y); break;
            case InputQueue::Event::PTR_AXIS:    injector_.InjectPointerAxis(ev.axis, ev.axis_value); break;
            case InputQueue::Event::KBD_ENTER:   injector_.InjectKeyboardEnter(ev.tl, ev.surface); break;
            case InputQueue::Event::KBD_LEAVE:   injector_.InjectKeyboardLeave(); break;
            case InputQueue::Event::KBD_KEY:     injector_.InjectKeyboardKey(ev.btn_or_key, ev.state); break;
            case InputQueue::Event::KBD_MODIFIERS: injector_.InjectKeyboardModifiers(ev.mod_depressed, ev.mod_latched, ev.mod_locked, ev.mod_group); break;
        }
    }
    // wl_display_flush_clients (display 在队列层初始化时注入)
    queue_.FlushClients();
}

// ========================================================================
//  Wayland 线程注入入口 — 公开委托 InputInjector (4C2 拆层; wl_core.cpp
//  首帧 focus 预设与 FlushQueue dispatch 调用, 签名/线程/语义逐字不变)
// ========================================================================

void InputManager::InjectPointerEnter(uint32_t tl, wl_resource* surface, wl_fixed_t sx, wl_fixed_t sy) {
    injector_.InjectPointerEnter(tl, surface, sx, sy);
}
void InputManager::InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy) {
    injector_.InjectPointerMotion(sx, sy);
}
void InputManager::InjectRelativeMotion(wl_fixed_t dx, wl_fixed_t dy) {
    injector_.InjectRelativeMotion(dx, dy);
}
void InputManager::InjectPointerButton(uint32_t button, uint32_t state) {
    injector_.InjectPointerButton(button, state);
}
void InputManager::InjectPointerAxis(int axis, wl_fixed_t value) {
    injector_.InjectPointerAxis(axis, value);
}
void InputManager::InjectPointerLeave() {
    injector_.InjectPointerLeave();
}
void InputManager::InjectKeyboardEnter(uint32_t tl, wl_resource* surface) {
    injector_.InjectKeyboardEnter(tl, surface);
}
void InputManager::InjectKeyboardKey(uint32_t key, uint32_t state) {
    injector_.InjectKeyboardKey(key, state);
}
void InputManager::InjectKeyboardLeave() {
    injector_.InjectKeyboardLeave();
}
void InputManager::InjectKeyboardModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    injector_.InjectKeyboardModifiers(depressed, latched, locked, group);
}
