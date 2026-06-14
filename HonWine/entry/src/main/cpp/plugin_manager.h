#pragma once
#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include "egl_renderer.h"
#include <unordered_map>
#include <set>
#include <memory>
#include <cstdint>

// XComponent 生命周期管理, 创建/销毁 EglRenderer
// 支持多窗口: 主窗口 primary renderer + 每个 toplevel 一个 shared renderer
class PluginManager {
public:
    static PluginManager* GetInstance();
    void Export(napi_env env, napi_value exports);

    // 多窗口: 设置待处理的 toplevel (ArkTS 在创建子窗口前调用)
    void SetPendingToplevel(uint32_t toplevelId) { pendingToplevelId_ = toplevelId; }
    void DestroyToplevel(uint32_t toplevelId);
    EglRenderer* GetMainRenderer();

    static void OnSurfaceCreated(OH_NativeXComponent*, void* window);
    static void OnSurfaceChanged(OH_NativeXComponent*, void* window);
    static void OnSurfaceDestroyed(OH_NativeXComponent*, void*);
    static void DispatchTouchEvent(OH_NativeXComponent*, void*) {}

private:
    PluginManager() = default;

    // 主渲染器 (单窗口兼容模式, 无 toplevelId)
    std::unique_ptr<EglRenderer> mainRenderer_;
    // toplevel 渲染器 (多窗口模式, 共享 mainRenderer_ 的 EGL context)
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;
    // 待处理的 toplevel: ArkTS 设置, 下一个 XComponent surface 创建时使用
    uint32_t pendingToplevelId_ = 0;

    // 区分主/子 XComponent: 第一个注册的是主窗口, 后续是子窗口
    OH_NativeXComponent* mainXComponent_ = nullptr;
    std::set<OH_NativeXComponent*> subXComponents_;

    OH_NativeXComponent_Callback callback_{};
};
