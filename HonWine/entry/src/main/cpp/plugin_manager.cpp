#include "plugin_manager.h"
#include <native_window/external_window.h>
#include <set>

#undef LOG_TAG
#define LOG_TAG "WL_Plugin"
#include <hilog/log.h>

PluginManager* PluginManager::GetInstance() {
    static PluginManager s;
    return &s;
}

void PluginManager::Export(napi_env env, napi_value exports) {
    napi_value xcVal = nullptr;
    napi_status st = napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcVal);
    OH_LOG_INFO(LOG_APP, "[MW-Export] status=%{public}d pendingToplevel=%{public}u", st, pendingToplevelId_);
    if (st != napi_ok) {
        OH_LOG_WARN(LOG_APP, "[Plugin] no XComponent in exports");
        return;
    }

    napi_valuetype type;
    napi_typeof(env, xcVal, &type);
    OH_LOG_INFO(LOG_APP, "[Plugin] XComponent type=%{public}d (napi_object=%{public}d)", type, napi_object);

    OH_NativeXComponent* nxc = nullptr;
    napi_status uw = napi_unwrap(env, xcVal, reinterpret_cast<void**>(&nxc));
    OH_LOG_INFO(LOG_APP, "[Plugin] napi_unwrap: status=%{public}d ptr=%{public}p", uw, nxc);

    if (uw != napi_ok || !nxc) {
        OH_LOG_WARN(LOG_APP, "[MW-Export] unwrap failed (status=%{public}d)", uw);
        return;
    }

    callback_.OnSurfaceCreated   = OnSurfaceCreated;
    callback_.OnSurfaceChanged   = OnSurfaceChanged;
    callback_.OnSurfaceDestroyed = OnSurfaceDestroyed;
    callback_.DispatchTouchEvent = DispatchTouchEvent;
    OH_NativeXComponent_RegisterCallback(nxc, &callback_);

    // 所有 XComponent 都属于子窗口 (主界面已无 XComponent)
    subXComponents_.insert(nxc);
    xcToToplevelId_[nxc] = pendingToplevelId_;
    OH_LOG_INFO(LOG_APP, "[MW-Export] XComponent registered: %{public}p → toplevel #%{public}u (total: %{public}zu)",
                nxc, pendingToplevelId_, subXComponents_.size());
}

void PluginManager::OnSurfaceCreated(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    auto* self = GetInstance();
    OH_LOG_INFO(LOG_APP, "[MW-Surface] created %{public}llux%{public}llu pendingToplevel=%{public}u",
                w, h, self->pendingToplevelId_);

    if (self->pendingToplevelId_ != 0) {
        // 子窗口: 独立 EGLContext (避免多线程共享 context 导致渲染交错)
        uint32_t tid = self->pendingToplevelId_;
        self->pendingToplevelId_ = 0;

        auto r = std::make_unique<EglRenderer>();
        r->Init(reinterpret_cast<OHNativeWindow*>(window), (int)w, (int)h);
        r->SetToplevelId(tid);
        self->toplevelRenderers_[tid] = std::move(r);
        OH_LOG_INFO(LOG_APP, "[MW-Surface] toplevel #%{public}u renderer created", tid);
    } else {
        // 无 toplevel = 旧主窗口 XComponent (已废弃, 不再使用)
        OH_LOG_WARN(LOG_APP, "[MW-Surface] no pending toplevel, skipping primary renderer");
    }
}

void PluginManager::OnSurfaceChanged(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    auto* self = GetInstance();
    OH_LOG_INFO(LOG_APP, "[MW-Surface] changed component=%{public}p size=%{public}llux%{public}llu",
                component, w, h);

    auto it = self->xcToToplevelId_.find(component);
    if (it != self->xcToToplevelId_.end()) {
        uint32_t tid = it->second;
        auto rit = self->toplevelRenderers_.find(tid);
        if (rit != self->toplevelRenderers_.end() && rit->second->IsValid()) {
            rit->second->SetSize((int)w, (int)h);
            OH_LOG_INFO(LOG_APP, "[MW-Surface] toplevel #%{public}u renderer resized to %{public}llux%{public}llu",
                        tid, w, h);
        }
    }
}

void PluginManager::OnSurfaceDestroyed(OH_NativeXComponent* component, void*) {
    auto* self = GetInstance();
    if (self->subXComponents_.count(component)) {
        OH_LOG_INFO(LOG_APP, "[MW-Destroy] XComponent destroyed: %{public}p (remaining: %{public}zu)",
                    component, self->subXComponents_.size() - 1);
        self->subXComponents_.erase(component);
        // toplevel renderer 由 ArkTS destroyToplevel 清理
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Destroy] UNKNOWN XComponent destroyed: %{public}p", component);
    }
}

void PluginManager::DestroyToplevel(uint32_t toplevelId) {
    auto it = toplevelRenderers_.find(toplevelId);
    if (it != toplevelRenderers_.end()) {
        it->second->Shutdown();
        toplevelRenderers_.erase(it);
        OH_LOG_INFO(LOG_APP, "[MW-Destroy] toplevel #%{public}u renderer destroyed (remaining: %{public}zu)",
                    toplevelId, toplevelRenderers_.size());
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Destroy] toplevel #%{public}u NOT found (remaining: %{public}zu)",
                    toplevelId, toplevelRenderers_.size());
    }
}
