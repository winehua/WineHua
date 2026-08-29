#pragma once
#include <cstdint>
#include <functional>

class ToplevelManager;
struct SurfaceData;

// 桌面 Root 窗口识别 + 切换逻辑 (从 surface_commit 中提取)。
// 状态 (desktopRootToplevelId_ 等) 仍由 WaylandServer 持有,
// 本类通过引用访问。这是有意为之: DesktopCompositor / InputResolver
// 也引用同一份状态, 不需要跨组件引用链。

class DesktopRootManager {
public:
    using FireEventFn = std::function<void(uint32_t, const char*, const char*)>;

    DesktopRootManager(ToplevelManager& tmgr,
                       uint32_t& desktopRootToplevelId,
                       uint32_t& pendingDesktopRootToplevelId,
                       bool& recognitionEnabled,
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
    uint32_t& desktopRootToplevelId_;
    uint32_t& pendingDesktopRootToplevelId_;
    bool& recognitionEnabled_;
    FireEventFn fireEvent_;
};
