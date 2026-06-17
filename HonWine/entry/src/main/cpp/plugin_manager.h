#pragma once
#include "egl_renderer.h"
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <queue>

// surfaceId 驱动的 XComponent 管理
//
// 每个 XComponent 通过自定义 Controller 回调拿到的 surfaceId 是唯一的,
// 不再共享 libraryname → exports 对象, 从根本上消除多窗口 XComponent 冲突。
//
// 架构:
//   WineWindow.ets (XComponentController.onSurfaceCreated)
//     → NAPI createRenderer(toplevelId, surfaceId)
//     → PluginManager::CreateRenderer → OH_NativeWindow_CreateNativeWindowFromSurfaceId
//     → EglRenderer::Init(nativeWindow, 1, 1) → 共享 EGLDisplay + 独立 EGLContext
//   WineWindow.ets (XComponentController.onSurfaceChanged)
//     → NAPI resizeRenderer(toplevelId, w, h)
//     → PluginManager::ResizeRenderer → EglRenderer::SetSize
//   WineWindow.ets (XComponentController.onSurfaceDestroyed)
//     → NAPI destroyRenderer(toplevelId) → DestroyToplevel
//
// 键盘事件仅通过 Stack.onKeyEvent → NAPI sendKeyEvent → InputManager 路径,
// 不再使用 OH_NativeXComponent_RegisterKeyEventCallback。
class PluginManager {
public:
    static PluginManager* GetInstance();

    // surfaceId 驱动的渲染器生命周期
    void CreateRenderer(uint32_t toplevelId, int64_t surfaceId);
    void ResizeRenderer(uint32_t toplevelId, int w, int h);
    void DestroyToplevel(uint32_t toplevelId);

    // pending toplevelId 队列: Ability 在 loadContent 前入队
    // WineWindow.aboutToAppear 同步出队 (FIFO, 无竞态)
    void SetPendingToplevel(uint32_t id) { pendingToplevelQueue_.push(id); }
    uint32_t DequeuePendingToplevel();

    // 辅助: toplevelId -> EglRenderer 查找 (InputManager 坐标转换使用)
    EglRenderer* GetRendererForToplevel(uint32_t tid);

private:
    PluginManager() = default;

    // 每个 toplevel 一个独立 EGLContext 渲染器
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;

    // pending queue: Ability 入队, WineWindow.aboutToAppear 出队 (FIFO 无竞态)
    std::queue<uint32_t> pendingToplevelQueue_;
};
