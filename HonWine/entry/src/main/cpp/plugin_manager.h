#pragma once
#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include "egl_renderer.h"
#include <unordered_map>
#include <set>
#include <memory>
#include <cstdint>

// XComponent 生命周期管理, 为每个 toplevel 创建独立的 EglRenderer
class PluginManager {
public:
    static PluginManager* GetInstance();
    void Export(napi_env env, napi_value exports);

    void SetPendingToplevel(uint32_t toplevelId) { pendingToplevelId_ = toplevelId; }
    void DestroyToplevel(uint32_t toplevelId);

    static void OnSurfaceCreated(OH_NativeXComponent*, void* window);
    static void OnSurfaceChanged(OH_NativeXComponent*, void* window);
    static void OnSurfaceDestroyed(OH_NativeXComponent*, void*);
    static void DispatchTouchEvent(OH_NativeXComponent*, void*) {}

private:
    PluginManager() = default;

    // 每个 toplevel 一个独立 EGLContext 渲染器
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;
    // ArkTS 在创建子窗口前调用 setPendingToplevel, native 在 OnSurfaceCreated 消费
    uint32_t pendingToplevelId_ = 0;

    // 跟踪 XComponent → toplevelId 映射
    std::set<OH_NativeXComponent*> subXComponents_;
    std::unordered_map<OH_NativeXComponent*, uint32_t> xcToToplevelId_;

    OH_NativeXComponent_Callback callback_{};
};
