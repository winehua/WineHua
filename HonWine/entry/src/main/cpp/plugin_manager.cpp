#include "plugin_manager.h"
#include "wayland_server.h"
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
    OH_LOG_INFO(LOG_APP, "[MW-Export] ▶ status=%{public}d pendingToplevel=%{public}u existXC=%{public}zu existRender=%{public}zu",
                st, pendingToplevelId_, subXComponents_.size(), toplevelRenderers_.size());
    if (st != napi_ok) {
        OH_LOG_WARN(LOG_APP, "[MW-Export] ✗ no XComponent in exports (status=%{public}d)", st);
        return;
    }

    napi_valuetype type;
    napi_typeof(env, xcVal, &type);
    OH_LOG_INFO(LOG_APP, "[MW-Export] XComponent type=%{public}d (napi_object=%{public}d)", type, napi_object);

    OH_NativeXComponent* nxc = nullptr;
    napi_status uw = napi_unwrap(env, xcVal, reinterpret_cast<void**>(&nxc));
    OH_LOG_INFO(LOG_APP, "[MW-Export] napi_unwrap: status=%{public}d ptr=%{public}p", uw, nxc);

    if (uw != napi_ok || !nxc) {
        OH_LOG_WARN(LOG_APP, "[MW-Export] ✗ unwrap failed (status=%{public}d)", uw);
        return;
    }

    // 检查是否已注册 (防止重复注册)
    if (subXComponents_.count(nxc)) {
        OH_LOG_WARN(LOG_APP, "[MW-Export] ⚠ XComponent %{public}p ALREADY registered (pendingToplevel=%{public}u), updating mapping",
                    nxc, pendingToplevelId_);
    }

    callback_.OnSurfaceCreated   = OnSurfaceCreated;
    callback_.OnSurfaceChanged   = OnSurfaceChanged;
    callback_.OnSurfaceDestroyed = OnSurfaceDestroyed;
    callback_.DispatchTouchEvent = DispatchTouchEvent;
    OH_NativeXComponent_RegisterCallback(nxc, &callback_);

    // 所有 XComponent 都属于子窗口 (主界面已无 XComponent)
    subXComponents_.insert(nxc);
    xcToToplevelId_[nxc] = pendingToplevelId_;
    OH_LOG_INFO(LOG_APP, "[MW-Export] ✓ XComponent %{public}p → toplevel #%{public}u (total xc=%{public}zu renderers=%{public}zu)",
                nxc, pendingToplevelId_, subXComponents_.size(), toplevelRenderers_.size());
}

void PluginManager::OnSurfaceCreated(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    auto* self = GetInstance();
    uint32_t tid = self->pendingToplevelId_;
    OH_LOG_INFO(LOG_APP, "[MW-Surface] created %{public}llux%{public}llu pendingToplevel=%{public}u window=%{public}p component=%{public}p",
                w, h, tid, window, component);

    if (tid != 0) {
        // 子窗口: 独立 EGLContext
        self->pendingToplevelId_ = 0;

        auto r = std::make_unique<EglRenderer>();
        OH_LOG_INFO(LOG_APP, "[MW-Surface] creating EglRenderer for toplevel #%{public}u (%{public}llux%{public}llu)...", tid, w, h);
        if (r->Init(reinterpret_cast<OHNativeWindow*>(window), (int)w, (int)h)) {
            r->SetToplevelId(tid);
            self->toplevelRenderers_[tid] = std::move(r);
            OH_LOG_INFO(LOG_APP, "[MW-Surface] ✓ toplevel #%{public}u renderer created (total=%{public}zu)", tid, self->toplevelRenderers_.size());
            // ★ 通知 ArkTS surface 的物理像素尺寸, 用于动态计算标题栏高度
            char json[64];
            snprintf(json, sizeof(json), "{\"w\":%llu,\"h\":%llu}", w, h);
            WaylandServer::GetInstance()->FireToplevelEvent(tid, "surface", json);
        } else {
            OH_LOG_ERROR(LOG_APP, "[MW-Surface] ✗ toplevel #%{public}u EglRenderer::Init FAILED", tid);
        }
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Surface] no pending toplevel (tid=0), skipping — old main XComponent?");
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
            // ★ 每次 surface 大小变化都回报给 ArkTS, 用于 titleBarH 校准
            char json[64];
            snprintf(json, sizeof(json), "{\"w\":%llu,\"h\":%llu}", w, h);
            WaylandServer::GetInstance()->FireToplevelEvent(tid, "surface", json);
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

    // 清理残留的 XComponent → toplevelId 映射
    for (auto xit = xcToToplevelId_.begin(); xit != xcToToplevelId_.end(); ) {
        if (xit->second == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW-Destroy] cleaned stale xcToToplevel: %{public}p → #%{public}u",
                        xit->first, xit->second);
            xit = xcToToplevelId_.erase(xit);
        } else {
            ++xit;
        }
    }
}
