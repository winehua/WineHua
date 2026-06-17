#pragma once
#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include "egl_renderer.h"
#include <unordered_map>
#include <set>
#include <memory>
#include <cstdint>

// XComponent 生命周期管理, 为每个 toplevel 创建独立的 EglRenderer
// 输入事件已移至 InputManager 统一管理
class PluginManager {
public:
    static PluginManager* GetInstance();
    void Export(napi_env env, napi_value exports);

    void SetPendingToplevel(uint32_t toplevelId) { pendingToplevelId_ = toplevelId; currentToplevelId_ = toplevelId; }
    uint32_t GetCurrentToplevelId() const { return currentToplevelId_; }
    void DestroyToplevel(uint32_t toplevelId);

    static void OnSurfaceCreated(OH_NativeXComponent*, void* window);
    static void OnSurfaceChanged(OH_NativeXComponent*, void* window);
    static void OnSurfaceDestroyed(OH_NativeXComponent*, void*);

    // 辅助: toplevelId -> EglRenderer 查找 (InputManager 坐标转换使用)
    EglRenderer* GetRendererForToplevel(uint32_t tid);

    // Native 键盘回调: XComponent 会抢占焦点, Stack.onKeyEvent 不触发
    // 必须走 native callback 接收键盘事件, 转发到 InputManager
    static void OnNativeKeyEvent(OH_NativeXComponent*, void*);

private:
    PluginManager() = default;

    // 每个 toplevel 一个独立 EGLContext 渲染器
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;
    uint32_t pendingToplevelId_ = 0;
    uint32_t currentToplevelId_ = 0;

    // 跟踪 XComponent -> toplevelId 映射
    std::set<OH_NativeXComponent*> subXComponents_;
    std::unordered_map<OH_NativeXComponent*, uint32_t> xcToToplevelId_;

    OH_NativeXComponent_Callback callback_{};
};
