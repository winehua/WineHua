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
        OH_LOG_INFO(LOG_APP, "[MW-Export] SUB XComponent registered: %{public}p (total subs: %{public}zu)",
                    nxc, subXComponents_.size());
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
        // 子窗口: 创建 shared renderer (共享主窗口 EGL context)
        uint32_t tid = self->pendingToplevelId_;
        self->pendingToplevelId_ = 0;

        auto* main = self->GetMainRenderer();
        if (main && main->IsValid()) {
            auto r = std::make_unique<EglRenderer>();
            r->InitShared(main->GetDisplay(), main->GetContext(),
                          reinterpret_cast<OHNativeWindow*>(window), (int)w, (int)h, tid);
            self->toplevelRenderers_[tid] = std::move(r);
            OH_LOG_INFO(LOG_APP, "[MW-Surface] toplevel #%{public}u renderer created (SHARED context)", tid);
        } else {
            OH_LOG_WARN(LOG_APP, "[MW-Surface] main NOT ready, standalone renderer for toplevel #%{public}u", tid);
            auto r = std::make_unique<EglRenderer>();
            r->Init(reinterpret_cast<OHNativeWindow*>(window), (int)w, (int)h);
            r->SetToplevelId(tid);
            self->toplevelRenderers_[tid] = std::move(r);
        }
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

void PluginManager::OnSurfaceChanged(OH_NativeXComponent*, void*) {}

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
