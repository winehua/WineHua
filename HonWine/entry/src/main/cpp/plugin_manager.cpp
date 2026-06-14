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

    // 跟踪所有已注册的 XComponent (主窗口 + 子窗口)
    if (!mainXComponent_) {
        mainXComponent_ = nxc;
        OH_LOG_INFO(LOG_APP, "[MW-Export] MAIN XComponent registered: %{public}p", nxc);
    } else {
        subXComponents_.insert(nxc);
        // 存储 XComponent → toplevelId 映射, OnSurfaceChanged 时用于查找 renderer
        xcToToplevelId_[nxc] = pendingToplevelId_;
        OH_LOG_INFO(LOG_APP, "[MW-Export] SUB XComponent registered: %{public}p → toplevel #%{public}u (total subs: %{public}zu)",
                    nxc, pendingToplevelId_, subXComponents_.size());
    }
}

void PluginManager::OnSurfaceCreated(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    auto* self = GetInstance();
    OH_LOG_INFO(LOG_APP, "[MW-Surface] created %{public}llux%{public}llu pendingToplevel=%{public}u mainRndr=%{public}s",
                w, h, self->pendingToplevelId_,
                (self->mainRenderer_ && self->mainRenderer_->IsValid()) ? "yes" : "no");

    if (self->pendingToplevelId_ != 0) {
        // 子窗口: 独立 EGLContext (避免多线程共享 context 导致渲染交错)
        uint32_t tid = self->pendingToplevelId_;
        self->pendingToplevelId_ = 0;

        auto r = std::make_unique<EglRenderer>();
        r->Init(reinterpret_cast<OHNativeWindow*>(window), (int)w, (int)h);
        r->SetToplevelId(tid);
        self->toplevelRenderers_[tid] = std::move(r);
        OH_LOG_INFO(LOG_APP, "[MW-Surface] toplevel #%{public}u renderer created (INDEPENDENT context)", tid);
    } else {
        // 主窗口: 创建 primary renderer (仅当不存在时)
        if (self->mainRenderer_ && self->mainRenderer_->IsValid()) {
            OH_LOG_WARN(LOG_APP, "[MW-Surface] main renderer ALREADY exists, skipping duplicate!");
            return;
        }
        auto r = std::make_unique<EglRenderer>();
        r->Init(reinterpret_cast<OHNativeWindow*>(window), (int)w, (int)h);
        self->mainRenderer_ = std::move(r);
        OH_LOG_INFO(LOG_APP, "[MW-Surface] PRIMARY renderer created ✓");
    }
}

void PluginManager::OnSurfaceChanged(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    auto* self = GetInstance();
    OH_LOG_INFO(LOG_APP, "[MW-Surface] changed component=%{public}p size=%{public}llux%{public}llu",
                component, w, h);

    if (component == self->mainXComponent_) {
        if (self->mainRenderer_ && self->mainRenderer_->IsValid()) {
            self->mainRenderer_->SetSize((int)w, (int)h);
            OH_LOG_INFO(LOG_APP, "[MW-Surface] main renderer resized to %{public}llux%{public}llu", w, h);
        }
    } else {
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
}

void PluginManager::OnSurfaceDestroyed(OH_NativeXComponent* component, void*) {
    auto* self = GetInstance();
    OH_LOG_INFO(LOG_APP, "[MW-Destroy] component=%{public}p (main=%{public}p subMatch=%{public}s)",
                component, self->mainXComponent_,
                self->subXComponents_.count(component) ? "yes" : "no");

    if (component == self->mainXComponent_) {
        OH_LOG_WARN(LOG_APP, "[MW-Destroy] MAIN XComponent destroyed! toplevels=%{public}zu",
                    self->toplevelRenderers_.size());
        if (self->mainRenderer_ && self->mainRenderer_->IsValid()) {
            self->mainRenderer_->Shutdown();
            self->mainRenderer_.reset();
        }
    } else if (self->subXComponents_.count(component)) {
        OH_LOG_INFO(LOG_APP, "[MW-Destroy] SUB XComponent destroyed: %{public}p", component);
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

EglRenderer* PluginManager::GetMainRenderer() {
    if (mainRenderer_ && mainRenderer_->IsValid()) return mainRenderer_.get();
    return nullptr;
}
