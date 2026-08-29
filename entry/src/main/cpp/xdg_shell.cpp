#include <wayland-server-core.h>
#include "include/xdg-shell-server-protocol.h"
#include "wayland_server.h"
#include "xdg_shell.h"
#include "xdg_configure.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <cstdio>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Xdg"
#include <hilog/log.h>

namespace {

// -- xdg_toplevel 实现 (最小: 记录 title, 其余空) --
static void tl_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void tl_resource_destroy(wl_resource* r) {
    // 使用 ToplevelData 独立的 toplevelId，不依赖 XdgSurface
    // 避免 wl_client_destroy 时 xs_resource_destroy 先释放 XdgSurface 导致野指针
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(r));
    if (td && td->toplevelId) {
        WaylandServer::GetInstance()->UnregisterToplevelResource(td->toplevelId);
    }
    delete td;
}
static void tl_set_parent(wl_client*, wl_resource*, wl_resource*) {}
static void tl_set_title(wl_client*, wl_resource* tlRes, const char* title) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    OH_LOG_INFO(LOG_APP, "[XDG] title tl=%{public}u %{public}s",
                sd->toplevelId, title ? title : "(null)");
    sd->title = title ? title : "";
    char json[512];
    snprintf(json, sizeof(json), "{\"title\":\"%s\"}", sd->title.c_str());
    WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "title", json);
}
static void tl_set_app_id(wl_client*, wl_resource* tlRes, const char* appId) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    OH_LOG_INFO(LOG_APP, "[XDG] app_id tl=%{public}u %{public}s",
                sd->toplevelId, appId ? appId : "(null)");
    sd->appId = appId ? appId : "";
}
static void tl_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
static void tl_move(wl_client*, wl_resource* tlRes, wl_resource* /*seat*/, uint32_t serial) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || td->toplevelId == 0) return;
    WaylandServer::GetInstance()->StartMoveGrab(td->toplevelId, serial);
}
static void tl_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
static void fire_limits_event(SurfaceData* sd) {
    if (!sd || sd->toplevelId == 0) return;
    char json[128];
    snprintf(json, sizeof(json),
             "{\"minW\":%d,\"minH\":%d,\"maxW\":%d,\"maxH\":%d}",
             sd->minWidth, sd->minHeight, sd->maxWidth, sd->maxHeight);
    OH_LOG_INFO(LOG_APP, "[XDG] fire_limits tl=%{public}u %{public}s maxState=%{public}s",
                sd->toplevelId, json,
                WaylandServer::GetInstance()->IsToplevelMaximized(sd->toplevelId) ? "yes" : "no");
    WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "limits", json);
}

static void tl_set_min_size(wl_client*, wl_resource* tlRes, int32_t w, int32_t h) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    sd->minWidth = w;
    sd->minHeight = h;
    sd->hasSizeLimits = true;
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_min_size toplevel=%{public}u %{public}dx%{public}d",
                sd->toplevelId, w, h);
    fire_limits_event(sd);
}

// Wine 最大化时不调 xdg_toplevel.set_maximized, 只把 max_size 设为
// 工作区尺寸 → compositor 以此推断 maximize 意图, 主动补发 MAXIMIZED
// configure (否则 Wine 永远等不到最大化确认)。
// 全屏窗口排除在外: 全屏时 Wine 会把 min/max 设为输出尺寸, 若触发本启发式
// 会补发一个工作区尺寸的 MAXIMIZED configure, 与 FULLSCREEN configure 打架,
// 使窗口落入 max+fs 混合态 (实测游戏全屏后 client 变成 1400x900, 画面下移)。
static bool ShouldInferMaximizeFromMaxSize(WaylandServer* ws, SurfaceData* sd,
                                           int32_t w, int32_t h, int32_t workH) {
    return !ws->IsToplevelFullscreen(sd->toplevelId) && !ws->IsToplevelMaximized(sd->toplevelId) &&
           w >= ws->outputW_ && h >= workH &&
           sd->toplevelId != ws->GetDesktopRootToplevelId();
}

static void tl_set_max_size(wl_client* client, wl_resource* tlRes, int32_t w, int32_t h) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    sd->maxWidth = w;
    sd->maxHeight = h;
    sd->hasSizeLimits = true;

    auto* ws = WaylandServer::GetInstance();
    int32_t workH = ws->GetWorkAreaHeight();
    if (ShouldInferMaximizeFromMaxSize(ws, sd, w, h, workH)) {
        sd->preMaxW = ws->GetToplevelW(sd->toplevelId);
        sd->preMaxH = ws->GetToplevelH(sd->toplevelId);
        ws->SetToplevelMaximizedState(sd->toplevelId, true);  // 权威在 ToplevelState (重构第 5C 步)
        ws->SetToplevelMaximized(sd->toplevelId);
        XdgConfigureSend(tlRes, xdg->xdgSurface, w, workH,
                         {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED});
        OH_LOG_INFO(LOG_APP, "[XDG] max_size→maximize tl=%{public}u → configure(%{public}d,%{public}d)",
                    sd->toplevelId, w, workH);
    } else {
        OH_LOG_INFO(LOG_APP, "[XDG] tl_set_max_size toplevel=%{public}u %{public}dx%{public}d",
                    sd->toplevelId, w, h);
    }
    fire_limits_event(sd);
}
static void tl_set_maximized(wl_client* client, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    auto* ws = WaylandServer::GetInstance();
    // 全屏窗口不接受最大化: MAXIMIZED configure 会与 FULLSCREEN 打架
    if (ws->IsToplevelFullscreen(sd->toplevelId)) {
        OH_LOG_INFO(LOG_APP, "[XDG] tl_set_maximized tl=%{public}u ignored (fullscreen)", sd->toplevelId);
        return;
    }
    if (ws->IsToplevelMinimized(sd->toplevelId)) {
        ws->SetToplevelRestored(sd->toplevelId);
    }
    if (!ws->IsToplevelMaximized(sd->toplevelId)) {  // 权威在 ToplevelState (重构第 5C 步)
        sd->preMaxW = ws->GetToplevelW(sd->toplevelId);
        sd->preMaxH = ws->GetToplevelH(sd->toplevelId);
        ws->SetToplevelMaximizedState(sd->toplevelId, true);
        ws->SetToplevelMaximized(sd->toplevelId);
    }
    // 发 configure 让 Wine 渲染到工作区尺寸 (排除任务栏)
    int32_t mw = ws->outputW_, mh = ws->GetWorkAreaHeight();
    XdgConfigureSend(tlRes, xdg->xdgSurface, mw, mh,
                     {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED});
    ws->FireToplevelEvent(sd->toplevelId, "maximized");
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_maximized tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, mw, mh);
}
static void tl_unset_maximized(wl_client* client, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    // 全屏窗口不接受 unmaximize: 否则会发出不带 FULLSCREEN 状态的 configure,
    // 把 Wine 的窗口状态机打出全屏
    if (WaylandServer::GetInstance()->IsToplevelFullscreen(sd->toplevelId)) {
        OH_LOG_INFO(LOG_APP, "[XDG] tl_unset_maximized tl=%{public}u ignored (fullscreen)", sd->toplevelId);
        return;
    }
    // maximized 状态位清 (权威在 ToplevelState, 重构第 5C 步): 裸状态写无
    // dirty — 还原到 preMax 尺寸后位置回归正常浮动, 合成更新由 Wine 随
    // configure 后的新帧 commit 触发 (与旧 sd->maximized = false 等价)
    WaylandServer::GetInstance()->SetToplevelMaximizedState(sd->toplevelId, false);
    // 发 configure 用最大化前尺寸, 不能用 0,0 (Wine 0,0+state → SWP_NOSIZE → 不resize)
    int32_t w = sd->preMaxW > 0 ? sd->preMaxW : 0;
    int32_t h = sd->preMaxH > 0 ? sd->preMaxH : 0;
    XdgConfigureSend(tlRes, xdg->xdgSurface, w, h, {XDG_TOPLEVEL_STATE_ACTIVATED});
    WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "unmaximized");
    OH_LOG_INFO(LOG_APP, "[XDG] tl_unset_maximized tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, w, h);
}
static void tl_set_fullscreen(wl_client* client, wl_resource* tlRes, wl_resource*) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    auto* ws = WaylandServer::GetInstance();

    if (ws->IsToplevelMinimized(sd->toplevelId)) {
        ws->SetToplevelRestored(sd->toplevelId);
    }
    if (!ws->IsToplevelFullscreen(sd->toplevelId)) {
        sd->preFsW = ws->GetToplevelW(sd->toplevelId);
        sd->preFsH = ws->GetToplevelH(sd->toplevelId);
        // 全屏 configure 不含 MAXIMIZED, Wine 会据此清掉 WS_MAXIMIZE;
        // 合成器侧的标志位必须同步清, 否则后续 configure 会持续误带 MAXIMIZED。
        // 重构第 5C 步: maximized 权威迁入 ToplevelState (本行原清
        // SurfaceData::maximized, 现清 ToplevelState — 唯一权威, 全屏生效
        // 状态 SetToplevelFullscreen 与 maximized 不再分存两处, 失步窗口消除)
        ws->SetToplevelMaximizedState(sd->toplevelId, false);
        ws->SetToplevelFullscreen(sd->toplevelId, true);
        // 全屏置顶 (RaiseToplevel 对全屏窗口跳过任务栏 pin)。
        // 注意走默认 userInitiated=false: 显示模式切换时 Wine 会批量连带
        // 标记旧窗口 fullscreen, 此处若取号, 全屏优先级就退回请求到达
        // 顺序决定论 (旧窗口压游戏, 见 ToplevelState::fsPriority 注释)
        ws->RaiseToplevel(sd->toplevelId);
    }
    // 按 xdg-shell 协议回 configure: FULLSCREEN 状态 + 整个输出尺寸
    // (含任务栏区, 全屏应覆盖)。Wine 可保持自己的分辨率不变 (fullscreen
    // 对任意尺寸兼容, 见 winewayland wayland_surface_config_is_compatible),
    // 缩放由合成器完成
    int32_t fw = ws->outputW_, fh = ws->outputH_;
    XdgConfigureSend(tlRes, xdg->xdgSurface, fw, fh,
                     {XDG_TOPLEVEL_STATE_FULLSCREEN, XDG_TOPLEVEL_STATE_ACTIVATED});
    ws->FireToplevelEvent(sd->toplevelId, "fullscreen");
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_fullscreen tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, fw, fh);
}
static void tl_unset_fullscreen(wl_client* client, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    auto* ws = WaylandServer::GetInstance();
    if (!ws->IsToplevelFullscreen(sd->toplevelId)) return;  // 非全屏: 幂等忽略

    ws->SetToplevelFullscreen(sd->toplevelId, false);
    // 恢复全屏前尺寸, 不能用 0,0 (Wine 0,0+state → SWP_NOSIZE → 不resize)
    int32_t w = sd->preFsW > 0 ? sd->preFsW : 0;
    int32_t h = sd->preFsH > 0 ? sd->preFsH : 0;
    XdgConfigureSend(tlRes, xdg->xdgSurface, w, h, {XDG_TOPLEVEL_STATE_ACTIVATED});
    ws->FireToplevelEvent(sd->toplevelId, "unfullscreen");
    OH_LOG_INFO(LOG_APP, "[XDG] tl_unset_fullscreen tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, w, h);
}
static void tl_set_minimized(wl_client*, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    // 生效状态唯一权威是 ToplevelState.minimized
    // (NotifyToplevelMinimized → SetToplevelMinimized 写入, 本处不再存协议侧副本)
    // 几何参数为历史遗留 (旧读 sd->geoX/geoY, 两参数在下方签名中未命名未消费);
    // 重构第 5A2 步 geo 字段消亡后改读快照 contentRect (窗口几何即时值, 语义
    // 与旧值同源) — 值仍未被消费, 仅保持调用形态并见证字段迁移
    WaylandServer::GetInstance()->NotifyToplevelMinimized(
        sd->toplevelId, sd->committed.contentRect.x, sd->committed.contentRect.y);
    WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "minimized");
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_minimized tl=%{public}u", sd->toplevelId);
}

static const struct xdg_toplevel_interface kToplevelImpl = {
    .destroy          = tl_destroy,
    .set_parent       = tl_set_parent,
    .set_title        = tl_set_title,
    .set_app_id       = tl_set_app_id,
    .show_window_menu = tl_show_window_menu,
    .move             = tl_move,
    .resize           = tl_resize,
    .set_max_size     = tl_set_max_size,
    .set_min_size     = tl_set_min_size,
    .set_maximized    = tl_set_maximized,
    .unset_maximized  = tl_unset_maximized,
    .set_fullscreen   = tl_set_fullscreen,
    .unset_fullscreen = tl_unset_fullscreen,
    .set_minimized    = tl_set_minimized,
};

// -- xdg_surface 实现 --
static void xs_destroy(wl_client*, wl_resource* r) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(r));
    OH_LOG_INFO(LOG_APP, "[MW-Life] xs_destroy xdg=%{public}p wlSurf=%{public}p",
                r, d ? d->wlSurface : nullptr);
    if (d && d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && sd->hasToplevel) {
            OH_LOG_INFO(LOG_APP, "[MW-Life] xs_destroy → OnToplevelDestroyed tl=%{public}u", sd->toplevelId);
            WaylandServer::GetInstance()->OnToplevelDestroyed(sd->toplevelId);
            WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "destroyed");
        }
    }
    wl_resource_destroy(r);
}

static void xs_get_toplevel(wl_client* client, wl_resource* xsRes, uint32_t id) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(xsRes));

    wl_resource* tl = wl_resource_create(client, &xdg_toplevel_interface,
                                          wl_resource_get_version(xsRes), id);
    d->xdgToplevel = tl;

    // 为 toplevel 创建独立的 user_data（ToplevelData），不再共享 XdgSurface*
    // 避免 wl_client_destroy 时 xs_resource_destroy 先释放 XdgSurface 导致 use-after-free
    auto* td = new ToplevelData();
    td->xdgSurface = xsRes;
    wl_resource_set_implementation(tl, &kToplevelImpl, td, tl_resource_destroy);

    // 关联 SurfaceData: 分配 toplevelId, 发送 created 事件到 ArkTS
    if (d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && !sd->hasToplevel) {
            sd->hasToplevel = true;
            // CommittedSurface.role 即时同步 (重构第 5A2 步): 角色判定单点 =
            // RoleFor (committed_surface.h), 取代旧"hasToplevel 猜义分流"
            sd->committed.role = RoleFor(sd->hasToplevel, sd->isSubsurface);
            sd->toplevelId = WaylandServer::GetInstance()->NextToplevelId();
            d->toplevelId = sd->toplevelId;
            td->toplevelId = sd->toplevelId;
            WaylandServer::GetInstance()->RegisterToplevelResource(sd->toplevelId, tl);
            // PC 模式: created 延迟到首帧 commit (此时才知 wl_shm 格式,
            // ARGB 异型窗口需走子窗口路线而非 ability, 见 surface_commit)
            if (!WaylandServer::GetInstance()->Policy().OhosWindowPerToplevel()) {
                WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "created",
                    "{\"w\":640,\"h\":480}");
            }
        }
    }

    // 发 toplevel configure (activated), 然后 xdg_surface configure
    XdgConfigureSend(tl, xsRes, 0, 0, {XDG_TOPLEVEL_STATE_ACTIVATED});
}

static void xs_get_popup(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*) {}
static void xs_set_window_geometry(wl_client*, wl_resource* xsRes, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(xsRes));
    if (!d || !d->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
    if (!sd) return;
    // CommittedSurface 几何写点直写 (重构第 5A2 步): 旧字段 geoX/geoY/geoW/geoH
    // (三义字段, PLAN §2.4) 已删除, window_geometry 原值直写内容矩形 —
    // 语义 (contentRect=buffer 内内容偏移, toplevel 的"桌面屏幕位置"义另经
    // compute 派生存入 screenPos) 与取值时机与旧"写 geo 字段→commit 时读"
    // 逐点等价, 含"写后未 commit 即被读"窗口期。
    sd->committed.hasWindowGeometry = true;
    sd->committed.contentRect.x = x;
    sd->committed.contentRect.y = y;
    sd->committed.contentRect.w = w;
    sd->committed.contentRect.h = h;
    OH_LOG_INFO(LOG_APP, "[MW-GEO] window_geometry for surface -> toplevel #%{public}u: (%{public}d,%{public}d %{public}dx%{public}d)",
                sd->toplevelId, x, y, w, h);
}
static void xs_ack_configure(wl_client*, wl_resource*, uint32_t) {}

static const struct xdg_surface_interface kSurfaceImpl = {
    .destroy             = xs_destroy,
    .get_toplevel        = xs_get_toplevel,
    .get_popup           = xs_get_popup,
    .set_window_geometry = xs_set_window_geometry,
    .ack_configure       = xs_ack_configure,
};

static void xs_resource_destroy(wl_resource* r) {
    // client 断开时 libwayland-server 走此路径, 不会触发 xs_destroy。
    // 必须在此处也做 compositor 清理 (等价于 xs_destroy 的逻辑),
    // 否则已断开进程的窗口像素永久滞留。
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(r));
    if (d && d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && sd->hasToplevel) {
            OH_LOG_INFO(LOG_APP, "[MW-Life] xs_resource_destroy → OnToplevelDestroyed tl=%{public}u (client disconnect)", sd->toplevelId);
            WaylandServer::GetInstance()->OnToplevelDestroyed(sd->toplevelId);
            WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "destroyed");
        }
    }
    delete static_cast<XdgSurface*>(wl_resource_get_user_data(r));
}

// -- xdg_wm_base 实现 --
static void wm_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void wm_create_positioner(wl_client* c, wl_resource*, uint32_t id) {
    wl_resource* p = wl_resource_create(c, &xdg_positioner_interface, 3, id);
    wl_resource_set_implementation(p, nullptr, nullptr, nullptr);
}

static void wm_get_xdg_surface(wl_client* client, wl_resource* wmRes,
                                uint32_t id, wl_resource* surfaceRes) {
    wl_resource* xs = wl_resource_create(client, &xdg_surface_interface,
                                          wl_resource_get_version(wmRes), id);
    auto* d = new XdgSurface();
    d->wlSurface = surfaceRes;
    d->xdgSurface = xs;
    wl_resource_set_implementation(xs, &kSurfaceImpl, d, xs_resource_destroy);
}

static void wm_pong(wl_client*, wl_resource*, uint32_t) {}

static const struct xdg_wm_base_interface kWmBaseImpl = {
    .destroy           = wm_destroy,
    .create_positioner = wm_create_positioner,
    .get_xdg_surface   = wm_get_xdg_surface,
    .pong              = wm_pong,
};

static void wm_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    uint32_t v = std::min(version, 3u);
    wl_resource* r = wl_resource_create(client, &xdg_wm_base_interface, v, id);
    wl_resource_set_implementation(r, &kWmBaseImpl, nullptr, nullptr);
}

} // namespace

extern "C" void RegisterXdgShell(wl_display* display) {
    wl_global_create(display, &xdg_wm_base_interface, 3, nullptr, wm_bind);
    OH_LOG_INFO(LOG_APP, "[XDG] wm_base global registered");
}
