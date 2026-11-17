#include "plugin_manager.h"
#include "compositor/wayland_server.h"
#include "display/x11_toplevel.h"
#include "display/display_session.h"
#include <native_window/external_window.h>
#include <algorithm>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Plugin"
#include <hilog/log.h>

PluginManager* PluginManager::GetInstance() {
    static PluginManager s;
    return &s;
}

uint32_t PluginManager::DequeuePendingToplevel() {
    if (pendingToplevelQueue_.empty()) return 0;
    uint32_t id = pendingToplevelQueue_.front();
    pendingToplevelQueue_.pop_front();
    return id;
}

// 清除队列中指定 id 的残坑: 窗口在 loadContent 完成前被销毁 (destroy-while-
// creating, aboutToAppear 未执行 → id 无人消费), 不删会让后续每个页面出队
// 拿到这个死 id → 渲染器挂错 toplevel 取不到帧 → 黑屏 (1.8 实测)。id 未在
// 队列 (页面已出队) 时是 no-op, 无条件调用安全。
void PluginManager::CancelPendingToplevel(uint32_t id) {
    const size_t before = pendingToplevelQueue_.size();
    pendingToplevelQueue_.erase(
        std::remove(pendingToplevelQueue_.begin(), pendingToplevelQueue_.end(), id),
        pendingToplevelQueue_.end());
    if (pendingToplevelQueue_.size() != before) {
        OH_LOG_WARN(LOG_APP, "[MW-Life] CancelPendingToplevel id=%{public}u (%{public}zu→%{public}zu)",
                    id, before, pendingToplevelQueue_.size());
    }
}

void PluginManager::CreateRenderer(uint32_t toplevelId, int64_t surfaceId) {
    if (winehua_x11_is_desktop(toplevelId)) {
        x11Surfaces_[toplevelId] = surfaceId;
        WineHua_DisplayRoute_StartWithSurface(surfaceId, false, 0, 0, true, false);
        return;
    }
    /* x11 多窗模式: per-xs 呈现归 x11_toplevel 映射层 (render pass 直画),
     * 不建 EglRenderer 不进 toplevelRenderers_ —— 只把 surfaceId 的
     * NativeWindow 回绑给映射层。spec §4。 */
    if (x11_toplevel_owns(toplevelId)) {
        OHNativeWindow* win = nullptr;
        int ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId((uint64_t)surfaceId, &win);
        if (ret != 0 || !win) {
            OH_LOG_ERROR(LOG_APP, "[MW-Create] x11 tl #%{public}u CreateNativeWindowFromSurfaceId FAILED ret=%{public}d",
                         toplevelId, ret);
            return;
        }
        /* I5 (review): attach 返回 false = toplevel 已死 (created 事件与
         * createRenderer 之间 xs 销毁的竞态) —— 窗当场销毁, 不留泄漏。 */
        if (!x11_toplevel_attach_window(toplevelId, win, 1, 1)) {
            OH_LOG_WARN(LOG_APP, "[MW-Create] x11 tl #%{public}u gone before attach, window destroyed", toplevelId);
            OH_NativeWindow_DestroyNativeWindow(win);
            return;
        }
        x11Surfaces_[toplevelId] = surfaceId;
        OH_LOG_INFO(LOG_APP, "[MW-Create] x11 tl #%{public}u window attached (no EglRenderer)", toplevelId);
        return;
    }

    OH_LOG_INFO(LOG_APP, "[MW-Life] CreateRenderer tl=%{public}u count=%{public}zu→%{public}zu",
                toplevelId, toplevelRenderers_.size(), toplevelRenderers_.size() + 1);
    OH_LOG_INFO(LOG_APP, "[MW-Create] toplevel=%{public}u surfaceId=%{public}ld existRender=%{public}zu",
                toplevelId, surfaceId, toplevelRenderers_.size());

    OHNativeWindow* win = nullptr;
    int ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId((uint64_t)surfaceId, &win);
    if (ret != 0 || !win) {
        OH_LOG_ERROR(LOG_APP, "[MW-Create] ERR toplevel #%{public}u CreateNativeWindowFromSurfaceId FAILED ret=%{public}d",
                     toplevelId, ret);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[MW-Create] native window created %{public}p for toplevel #%{public}u", win, toplevelId);

    // A failed replacement must not retire a healthy previous surface.
    auto old = toplevelRenderers_.find(toplevelId);
    if (old != toplevelRenderers_.end()) {
        old->second->Shutdown();
        toplevelRenderers_.erase(old);
    }

    // 构造注入 frame compositor (重构第 6A 步): 渲染器取帧/ZC 状态机经
    // DesktopCompositor 直连 — 替代旧 WaylandServer 门面 26 处转发
    // (TakeToplevelFrame/GetZeroCopyLayerInfo/ActivateZcSurface 等)。
    // compositor 生命周期长于一切 renderer (WaylandServer 单例成员, 装配出口
    // 见 wayland_server.h GetDesktopCompositor 注释), 无后置装配竞态。
    auto r = std::make_unique<EglRenderer>(
        WaylandServer::GetInstance()->GetDesktopCompositor());
    // 初始尺寸 1x1, 真正尺寸由 ResizeRenderer (onSurfaceChanged) 设置
    r->SetToplevelId(toplevelId);
    r->SetIsolatedToplevel(isolatedToplevels_.count(toplevelId) != 0);
    r->SetSurfaceId(surfaceId);
    const auto visibility = rendererPaused_.find(toplevelId);
    if (visibility != rendererPaused_.end()) r->SetPaused(visibility->second);
    if (r->Init(win, 1, 1)) {
        toplevelRenderers_[toplevelId] = std::move(r);
        WaylandServer::GetInstance()->ForceToplevelRedraw(toplevelId);
        OH_LOG_INFO(LOG_APP, "[MW-Create] OK toplevel #%{public}u renderer created (total=%{public}zu)",
                    toplevelId, toplevelRenderers_.size());
    } else {
        OH_LOG_ERROR(LOG_APP, "[MW-Create] ERR toplevel #%{public}u EglRenderer::Init FAILED", toplevelId);
        // Init owns win on all paths; the renderer destructor cleans it up.
    }
}

void PluginManager::SetRendererStretchFill(uint32_t toplevelId, bool on) {
    auto rit = toplevelRenderers_.find(toplevelId);
    if (rit != toplevelRenderers_.end() && rit->second->IsValid()) {
        rit->second->SetStretchFill(on);
    }
}

void PluginManager::ResizeRenderer(uint32_t toplevelId, int w, int h, int64_t surfaceId) {
    const auto x = x11Surfaces_.find(toplevelId);
    if (x != x11Surfaces_.end() && surfaceId && x->second != surfaceId) return;
    if (winehua_x11_is_desktop(toplevelId)) {
        winehua_x11_canvas_size(w, h);
        WineHua_DisplayRoute_CanvasResized();
        return;
    }
    if (w <= 0 || h <= 0) return;
    /* x11 多窗模式: 无 EglRenderer, 尺寸同步给映射层 (per-xs 呈现按队列
     * buffer 本体拉伸, 此处只记账)。 */
    if (x11_toplevel_owns(toplevelId)) {
        x11_toplevel_resize_window(toplevelId, w, h);
        return;
    }

    OH_LOG_INFO(LOG_APP, "[MW-Resize] toplevel=%{public}u size=%{public}dx%{public}d", toplevelId, w, h);

    auto rit = toplevelRenderers_.find(toplevelId);
    if (rit != toplevelRenderers_.end() && surfaceId && rit->second->GetSurfaceId() != surfaceId) return;
    if (surfaceId && (rit == toplevelRenderers_.end() || !rit->second->IsValid())) {
        CreateRenderer(toplevelId, surfaceId);
        rit = toplevelRenderers_.find(toplevelId);
    }
    if (rit != toplevelRenderers_.end() && rit->second->IsValid()) {
        rit->second->SetSize(w, h);
        OH_LOG_INFO(LOG_APP, "[MW-Resize] toplevel #%{public}u renderer resized OK", toplevelId);

        // surface 尺寸变化后强制重绘: 即使没有新帧, 也需用当前帧在
        // 新 surface 上重新 letterbox, 避免旧 viewport 残留导致黑边
        WaylandServer::GetInstance()->ForceToplevelRedraw(toplevelId);

        // 通知 ArkTS surface 的物理像素尺寸, 用于动态计算标题栏高度
        WaylandServer::GetInstance()->PostToplevelEvent(
            toplevelId, ToplevelEventType::Surface,
            ToplevelEventBus::JsonSurface(w, h));
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Resize] toplevel #%{public}u renderer NOT found or invalid", toplevelId);
    }
}

void PluginManager::DestroyToplevel(uint32_t toplevelId) {
    x11Surfaces_.erase(toplevelId);
    if (winehua_x11_is_desktop(toplevelId)) {
        WineHua_DisplayRoute_DetachCanvas();
        return;
    }
    rendererPaused_.erase(toplevelId);
    /* x11 多窗模式: 解绑回绑窗口后照走既有 ArkTS 关窗清理 (destroyed 事件
     * 由映射层发, 这里是 surface 销毁侧的对称操作)。 */
    if (x11_toplevel_owns(toplevelId)) {
        x11_toplevel_detach_window(toplevelId);
        return;
    }

    OH_LOG_INFO(LOG_APP, "[MW-Life] DestroyRenderer tl=%{public}u count=%{public}zu→%{public}zu",
                toplevelId, toplevelRenderers_.size(), toplevelRenderers_.size() > 0 ? toplevelRenderers_.size() - 1 : 0);
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

void PluginManager::DestroyRendererForSurface(uint32_t toplevelId, int64_t surfaceId) {
    if (!surfaceId) { DestroyToplevel(toplevelId); return; }
    const auto x = x11Surfaces_.find(toplevelId);
    if (x != x11Surfaces_.end()) {
        if (x->second == surfaceId) DestroyToplevel(toplevelId);
        return;
    }
    // Root promotion may change the owning toplevel between callbacks.
    for (const auto& entry : toplevelRenderers_) {
        if (entry.second->GetSurfaceId() == surfaceId) {
            DestroyToplevel(entry.first);
            return;
        }
    }
}

EglRenderer* PluginManager::GetAnyRenderer() {
    if (toplevelRenderers_.empty()) return nullptr;
    return toplevelRenderers_.begin()->second.get();
}

void PluginManager::MoveRendererToToplevel(uint32_t oldId, uint32_t newId) {
    OH_LOG_INFO(LOG_APP, "[MW-Life] MoveRenderer tl %{public}u→%{public}u", oldId, newId);
    if (oldId == newId) { OH_LOG_WARN(LOG_APP, "[MW-Life] MoveRenderer SKIP: old==new"); return; }
    auto it = toplevelRenderers_.find(oldId);
    if (it == toplevelRenderers_.end()) {
        OH_LOG_WARN(LOG_APP, "[MW-Life] MoveRenderer old tl=%{public}u NOT FOUND", oldId);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[MW-Plug] MoveRenderer tl #%{public}u -> #%{public}u", oldId, newId);
    auto existing = toplevelRenderers_.find(newId);
    if (existing != toplevelRenderers_.end()) existing->second->Shutdown();
    // Erase before insertion: operator[] may rehash and invalidate it.
    auto renderer = std::move(it->second);
    toplevelRenderers_.erase(it);
    const auto visibility = rendererPaused_.find(newId);
    if (visibility != rendererPaused_.end()) renderer->SetPaused(visibility->second);
    else {
        const auto oldVisibility = rendererPaused_.find(oldId);
        if (oldVisibility != rendererPaused_.end()) rendererPaused_[newId] = oldVisibility->second;
    }
    rendererPaused_.erase(oldId);
    renderer->SetToplevelId(newId);
    toplevelRenderers_[newId] = std::move(renderer);
}

// ---- VPP additions ----
void PluginManager::RefreshRenderer(uint32_t toplevelId) {
    auto it = toplevelRenderers_.find(toplevelId);
    if (it != toplevelRenderers_.end() && !it->second->IsValid()) {
        const auto surfaceId = it->second->GetSurfaceId();
        CreateRenderer(toplevelId, surfaceId);
        it = toplevelRenderers_.find(toplevelId);
    }
    if (it == toplevelRenderers_.end() || !it->second->IsValid()) {
        OH_LOG_WARN(LOG_APP, "[MW-Refresh] toplevel #%{public}u renderer NOT found or invalid", toplevelId);
        return;
    }

    // 压测路径绝不能模拟 onSurfaceDestroyed/onSurfaceCreated：频繁释放 EGL
    // Surface 会与 ArkUI 的真实生命周期竞争。请求 Wayland 将当前帧重新提交，
    // 能覆盖 compositor/renderer 刷新，同时保留同一个 NativeWindow 与 EGLContext。
    it->second->RequestRedraw();
    WaylandServer::GetInstance()->ForceToplevelRedraw(toplevelId);
}

void PluginManager::SetRendererPaused(uint32_t toplevelId, bool paused) {
    rendererPaused_[toplevelId] = paused;
    if (!paused) RefreshRenderer(toplevelId);
    auto it = toplevelRenderers_.find(toplevelId);
    if (it != toplevelRenderers_.end()) it->second->SetPaused(paused);
}

void PluginManager::SetToplevelIsolated(uint32_t id, bool isolated) {
    WaylandServer::GetInstance()->GetDesktopCompositor().SetToplevelIsolated(id, isolated);
    if (isolated) isolatedToplevels_[id] = true;
    else isolatedToplevels_.erase(id);
    auto it = toplevelRenderers_.find(id);
    if (it != toplevelRenderers_.end()) it->second->SetIsolatedToplevel(isolated);
}
