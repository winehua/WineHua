#include <wayland-server-core.h>
#include "include/xdg-shell-server-protocol.h"
#include "wayland_server.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <cstdio>

#undef LOG_TAG
#define LOG_TAG "WL_Xdg"
#include <hilog/log.h>

namespace {

struct XdgSurface {
    wl_resource* wlSurface = nullptr;
    wl_resource* xdgSurface = nullptr;
    wl_resource* xdgToplevel = nullptr;
    uint32_t toplevelId = 0;  // 在 xs_get_toplevel 时分配，供 toplevel 销毁回调使用
};

// Toplevel 独立的 user_data，不共享 XdgSurface*
// 解决 wl_client_destroy 时 xs_resource_destroy 先释放 XdgSurface，
// 然后 tl_resource_destroy 访问野指针的问题
struct ToplevelData {
    uint32_t toplevelId = 0;
    wl_resource* xdgSurface = nullptr;  // 回指针，供 tl_set_title 等操作
};

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
    OH_LOG_INFO(LOG_APP, "[XDG] title=%{public}s", title ? title : "(null)");
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    sd->title = title ? title : "";
    char json[512];
    snprintf(json, sizeof(json), "{\"title\":\"%s\"}", sd->title.c_str());
    WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "title", json);
}
static void tl_set_app_id(wl_client*, wl_resource*, const char* appId) {
    OH_LOG_INFO(LOG_APP, "[XDG] app_id=%{public}s", appId ? appId : "(null)");
}
static void tl_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
static void tl_move(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
static void tl_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
static void tl_set_max_size(wl_client*, wl_resource*, int32_t, int32_t) {}
static void tl_set_min_size(wl_client*, wl_resource*, int32_t, int32_t) {}
static void tl_set_maximized(wl_client*, wl_resource*) {}
static void tl_unset_maximized(wl_client*, wl_resource*) {}
static void tl_set_fullscreen(wl_client*, wl_resource*, wl_resource*) {}
static void tl_unset_fullscreen(wl_client*, wl_resource*) {}
static void tl_set_minimized(wl_client*, wl_resource*) {}

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
    if (d && d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && sd->hasToplevel) {
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

    // 关联 SurfaceData: 分配 toplevelId, 标记 hasToplevel
    if (d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && !sd->hasToplevel) {
            sd->hasToplevel = true;
            sd->toplevelId = WaylandServer::GetInstance()->NextToplevelId();
            d->toplevelId = sd->toplevelId;  // XdgSurface 也缓存 toplevelId
            td->toplevelId = sd->toplevelId; // ToplevelData 存储独立的拷贝
            OH_LOG_INFO(LOG_APP, "[MW] xs_get_toplevel -> id=%{public}u (first toplevel for this surface)", sd->toplevelId);
            // 注册 toplevel resource (用于 SendToplevelClose)
            WaylandServer::GetInstance()->RegisterToplevelResource(sd->toplevelId, tl);
            WaylandServer::GetInstance()->FireToplevelEvent(sd->toplevelId, "created",
                "{\"w\":640,\"h\":480}");
        }
    }

    // 发 toplevel configure (activated), 然后 xdg_surface configure
    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(tl, 0, 0, &states);
    wl_array_release(&states);

    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xsRes, wl_display_next_serial(dpy));
}

static void xs_get_popup(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*) {}
static void xs_set_window_geometry(wl_client*, wl_resource* xsRes, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(xsRes));
    if (!d || !d->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
    if (!sd) return;
    sd->hasWindowGeometry = true;
    sd->geoX = x;
    sd->geoY = y;
    sd->geoW = w;
    sd->geoH = h;
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
