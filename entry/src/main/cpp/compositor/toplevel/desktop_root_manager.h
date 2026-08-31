#pragma once
#include <cstdint>
#include <functional>

class ToplevelManager;
class DesktopSessionState;
struct SurfaceData;

// 桌面 Root 窗口识别 + 切换逻辑 (从 surface_commit 中提取)。
// 状态 (desktopRootToplevelId 等) 的存储 = DesktopSessionState POD
// (重构第 6B 步): 本类经构造注入的 DesktopSessionState& 真正拥有 root
// 状态 — 消除 6A 前"引用成员指向宿主子字段"的隐式同步 (旧形态: 持
// uint32_t& 直接指向 WaylandServer::desktopRootToplevelId_ 等三个子字段)。
// DesktopCompositor / InputResolver / PointerExtras / InputManager 仍经
// 同一 POD (它们的注入引用指向 session 字段) 读到本类写入的同值状态,
// 不再需要跨组件引用链。
//
// 锁域不变: 所有状态读写在 tmgr 锁内做 (CheckRootLocked/MarkRootDirtyLocked
// 调用方持锁; SetRecognitionEnabled 自取锁), 与迁入前逐字一致。

class DesktopRootManager {
public:
    using FireEventFn = std::function<void(uint32_t, const char*, const char*)>;

    DesktopRootManager(ToplevelManager& tmgr,
                       DesktopSessionState& session,
                       FireEventFn fireEvent);

    // root 识别开关 (ArkTS 端完成后调用)
    void SetRecognitionEnabled(bool enabled);

    // 提 pending root 为正式 root (ArkTS 端子窗口 ready 后调用)。
    // 返回 promoted toplevel ID (0 = 无需操作)。
    // 调用方负责: PluginManager::MoveRendererToToplevel(0, id)。
    // PostToplevelEvent 已由内部 fireEvent_ 回调处理 (重构第 5D 步)。
    uint32_t PromotePending();

    // surface_commit 中 root 识别决策树的结果
    struct CheckRootResult {
        uint32_t moveRendererFrom = 0;
        uint32_t moveRendererTo = 0;
        bool fireDesktopRoot = false;
    };

    // 检查新提交的 toplevel 是否应成为桌面 root。
    // 调用方须已持有 toplevelManager 的锁 (tmgr_.Lock())。
    // 调用后调用方负责: MarkDesktopRootDirtyLocked + 锁外 MoveRendererToToplevel / PostToplevelEvent。
    CheckRootResult CheckRootLocked(SurfaceData* sd, bool isFirstCommit);

    // 标记 root dirty (root 切换后调用, 调用方须已持有锁)
    void MarkRootDirtyLocked();

private:
    ToplevelManager& tmgr_;
    // 桌面会话状态 (唯一存储): 本类读写 desktopRootToplevelId /
    // pendingDesktopRootToplevelId / desktopRootRecognitionEnabled —
    // 与 WaylandServer 及其他消费者经同一 POD 存取 (重构第 6B 步)
    DesktopSessionState& state_;
    FireEventFn fireEvent_;
};
