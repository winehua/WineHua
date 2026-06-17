#include "plugin_manager.h"
#include "wayland_server.h"
#include <native_window/external_window.h>

#undef LOG_TAG
#define LOG_TAG "WL_Plugin"
#include <hilog/log.h>

PluginManager* PluginManager::GetInstance() {
    static PluginManager s;
    return &s;
}

uint32_t PluginManager::DequeuePendingToplevel() {
    if (pendingToplevelQueue_.empty()) return 0;
    uint32_t id = pendingToplevelQueue_.front();
    pendingToplevelQueue_.pop();
    return id;
}

void PluginManager::CreateRenderer(uint32_t toplevelId, int64_t surfaceId) {
    OH_LOG_INFO(LOG_APP, "[MW-Create] toplevel=%{public}u surfaceId=%{public}ld existRender=%{public}zu",
                toplevelId, surfaceId, toplevelRenderers_.size());

    // 防御: 如果已有旧 renderer, 先 shutdown
    auto old = toplevelRenderers_.find(toplevelId);
    if (old != toplevelRenderers_.end()) {
        OH_LOG_WARN(LOG_APP, "[MW-Create] WARN toplevel #%{public}u already has renderer, shutting down old", toplevelId);
        old->second->Shutdown();
        toplevelRenderers_.erase(old);
    }

    OHNativeWindow* win = nullptr;
    int ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId((uint64_t)surfaceId, &win);
    if (ret != 0 || !win) {
        OH_LOG_ERROR(LOG_APP, "[MW-Create] ERR toplevel #%{public}u CreateNativeWindowFromSurfaceId FAILED ret=%{public}d",
                     toplevelId, ret);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[MW-Create] native window created %{public}p for toplevel #%{public}u", win, toplevelId);

    auto r = std::make_unique<EglRenderer>();
    // 初始尺寸 1x1, 真正尺寸由 ResizeRenderer (onSurfaceChanged) 设置
    if (r->Init(win, 1, 1)) {
        r->SetToplevelId(toplevelId);
        toplevelRenderers_[toplevelId] = std::move(r);
        OH_LOG_INFO(LOG_APP, "[MW-Create] OK toplevel #%{public}u renderer created (total=%{public}zu)",
                    toplevelId, toplevelRenderers_.size());
    } else {
        OH_LOG_ERROR(LOG_APP, "[MW-Create] ERR toplevel #%{public}u EglRenderer::Init FAILED", toplevelId);
        // 销毁刚才创建的 native window
        OH_NativeWindow_DestroyNativeWindow(win);
    }
}

void PluginManager::ResizeRenderer(uint32_t toplevelId, int w, int h) {
    OH_LOG_INFO(LOG_APP, "[MW-Resize] toplevel=%{public}u size=%{public}dx%{public}d", toplevelId, w, h);

    auto rit = toplevelRenderers_.find(toplevelId);
    if (rit != toplevelRenderers_.end() && rit->second->IsValid()) {
        rit->second->SetSize(w, h);
        OH_LOG_INFO(LOG_APP, "[MW-Resize] toplevel #%{public}u renderer resized OK", toplevelId);

        // 通知 ArkTS surface 的物理像素尺寸, 用于动态计算标题栏高度
        char json[64];
        snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", w, h);
        WaylandServer::GetInstance()->FireToplevelEvent(toplevelId, "surface", json);
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Resize] toplevel #%{public}u renderer NOT found or invalid", toplevelId);
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

EglRenderer* PluginManager::GetRendererForToplevel(uint32_t tid) {
    auto rit = toplevelRenderers_.find(tid);
    if (rit == toplevelRenderers_.end()) return nullptr;
    return rit->second.get();
}
