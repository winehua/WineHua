// X11/GNU window bridge. Only NativeBuffer references cross the renderer boundary.
#include "x11_external_host.h"
#include "display_session.h"
#include "desktop_fit.h"
#include "ohos_output.h"
#include "ohos_buffer.h"
#include "ohos_egl_import.h"
#include "compositor/wayland_server.h"
#include "graphics/egl_renderer.h"
#include "input/input_manager.h"
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

extern "C" bool x11_toplevel_input_position(uint32_t, double, double, double*, double*);
namespace {
struct Window { winehua_x11_window info{}; bool productChild = false; };
struct Binding {
    uint32_t source = 0, host = 0, pid = 0;
    uint64_t sourceKey = 0, generation = 0;
    uint32_t xid = 0;
    int x = 0, y = 0, w = 0, h = 0;
    std::unique_ptr<EglRenderer> renderer;
    bool paused = false;
    OH_NativeImage* image = nullptr;
    ~Binding() { if (renderer) renderer->Shutdown(); if (image) OH_NativeImage_Destroy(&image); }
};
struct Request {
    uint32_t source, host, pid; uint64_t key, generation; uint32_t xid;
    int x, y, w, h;
};
std::unordered_map<uint32_t, Request> pending;
std::unordered_set<uint32_t> removals;
std::mutex mutex;
std::unordered_map<uint32_t, Window> windows;
std::unordered_map<uint32_t, std::unique_ptr<Binding>> bindings;
uint32_t grabbedHost = 0;
std::unordered_set<uint32_t> heldButtons;
uint64_t SourceKey(uint32_t id, uint32_t pid);
double grabbedX = 0, grabbedY = 0;
void ReleaseCapture(const Binding& b) {
    if (grabbedHost != b.host) return;
    // Host teardown must not leave a live GNU client with a pressed button.
    if (SourceKey(b.source, b.pid) == b.sourceKey)
        for (const auto button : heldButtons)
            InputManager::GetInstance()->SendPointerEvent(b.source, 2, grabbedX, grabbedY, button, 0, 0, true, true);
    grabbedHost = 0; heldButtons.clear();
}

uint64_t SourceKey(uint32_t id, uint32_t pid = 0) {
    auto& tm = WaylandServer::GetInstance()->GetToplevelManager();
    auto guard = tm.Lock();
    for (const auto& [key, resource] : tm.SurfaceResources()) {
        auto* sd = tm.FindRegisteredSurfaceDataLocked(resource);
        if (sd && sd->hasToplevel && sd->toplevelId == id && (!pid || sd->clientPid == pid)) return key;
    }
    return 0;
}
bool Live(const Binding& b) {
    const auto it = windows.find(b.host);
    return it != windows.end() && it->second.info.generation == b.generation &&
        it->second.info.xid == b.xid && SourceKey(b.source, b.pid) == b.sourceKey;
}
void ClearFrame(const Binding& b) {
    const auto it = windows.find(b.host);
    uint64_t generation = 0;
    if (it != windows.end() && it->second.info.generation == b.generation &&
        wl_ohos_output_client_xwindow_generation(b.xid, &generation) && generation == b.generation) {
        wl_ohos_output_client_frame_clear(b.xid);
        wl_ohos_output_client_frame_region(b.xid, 0, 0, 0, 0);
    }
}
bool GlobalPoint(uint32_t canvas, double px, double py, double& x, double& y) {
    if (!std::isfinite(px) || !std::isfinite(py)) return false;
    if (!winehua_x11_is_desktop(canvas)) return x11_toplevel_input_position(canvas, px, py, &x, &y);
    int cw, ch, ox, oy, w, h;
    winehua_x11_canvas_dimensions(&cw, &ch);
    if (!wl_ohos_output_desktop_rect(&ox, &oy, &w, &h)) return false;
    DesktopFitRect fit{};
    if (!WineHua_ComputeDesktopFit(cw, ch, w, h, &fit)) return false;
    x = WineHua_FitUnmapX(&fit, px) + ox;
    y = WineHua_FitUnmapY(&fit, py) + oy;
    return true;
}
Binding* Pick(uint32_t canvas, double px, double py, double& x, double& y) {
    if (!GlobalPoint(canvas, px, py, x, y)) return nullptr;
    Binding* picked = nullptr;
    for (auto& [host, item] : bindings) {
        auto& b = *item;
        if (!Live(b) || !windows.at(host).info.mapped || !b.renderer) continue;
        const auto& win = windows.at(host).info;
        const bool inside = x >= win.x + b.x && y >= win.y + b.y &&
            x < win.x + b.x + b.w && y < win.y + b.y + b.h;
        if ((grabbedHost && host == grabbedHost) || (!grabbedHost && inside && (canvas == host || winehua_x11_is_desktop(canvas)))) {
            // Never choose an arbitrary overlap: only one external binding per point is valid.
            if (picked && !grabbedHost) return nullptr;
            picked = &b;
        }
    }
    if (picked) {
        const auto& win = windows.at(picked->host).info;
        x -= win.x + picked->x; y -= win.y + picked->y;
    }
    return picked;
}
}
extern "C" bool winehua_x11_host_owns(uint32_t id) {
    std::lock_guard<std::mutex> guard(mutex); return windows.count(id) != 0;
}
extern "C" bool winehua_x11_bind_external(uint32_t source, uint32_t host, uint32_t sourcePid,
    uint32_t hostPid, int x, int y, int w, int h) {
    if (!source || !host || source == host || !sourcePid || !hostPid || x < 0 || y < 0 ||
        w < 1 || h < 1 || x > 4096 || y > 4096 || w > 4096 || h > 4096) return false;
    const uint64_t key = SourceKey(source, sourcePid);
    if (!key) return false;
    std::lock_guard<std::mutex> guard(mutex);
    const auto it = windows.find(host);
    if (it == windows.end() || it->second.info.pid != hostPid) return false;
    for (const auto& [otherHost, b] : bindings)
        if (otherHost != host && b->source == source) return false;
    for (const auto& [otherHost, req] : pending)
        if (otherHost != host && req.source == source) return false;
    auto existing = bindings.find(host);
    auto* b = existing == bindings.end() ? nullptr : existing->second.get();
    if (b && b->sourceKey == key && b->generation == it->second.info.generation &&
        b->x == x && b->y == y && b->w == w && b->h == h) return true;
    // Old producer remains owned until the compositor thread retires it.
    // Reconfiguration is a pending request, not destruction on the JS thread.
    pending[host] = {source, host, sourcePid, key, it->second.info.generation, it->second.info.xid, x, y, w, h};
    return true;
}

extern "C" void winehua_x11_unbind_external(uint32_t host) {
    std::lock_guard<std::mutex> guard(mutex);
    pending.erase(host); removals.insert(host);
}
extern "C" void winehua_x11_hosts_tick() {
    winehua_x11_window snapshot[256];
    const size_t n = wl_ohos_output_snapshot_windows(snapshot, 256);
    std::lock_guard<std::mutex> guard(mutex);
    std::unordered_set<uint32_t> seen;
    auto* server = WaylandServer::GetInstance();
    for (size_t i = 0; i < n; ++i) {
        auto info = snapshot[i];
        if (!info.pid || !info.xid) continue;
        Window* previous = nullptr;
        for (auto& [id, win] : windows) {
            if (win.info.xid == info.xid && win.info.generation == info.generation) { previous = &win; info.id = id; break; }
        }
        if (!previous) {
            const bool child = !info.id;
            if (child) info.id = server->GetToplevelManager().AllocateToplevelId();
            windows[info.id] = {info, child};
            if (child) server->PostToplevelEvent(info.id, ToplevelEventType::Created,
                ToplevelEventBus::JsonCreatedAt(info.w, info.h, info.x, info.y, info.pid));
        } else {
            const auto old = previous->info;
            previous->info = info;
            if (previous->productChild) {
                if (old.w != info.w || old.h != info.h) server->PostToplevelEvent(info.id, ToplevelEventType::Resize,
                    ToplevelEventBus::JsonResize(info.w, info.h));
                if (old.mapped != info.mapped) server->PostToplevelEvent(info.id,
                    info.mapped ? ToplevelEventType::Restored : ToplevelEventType::Minimized);
            }
        }
        seen.insert(info.id);
    }
    for (auto it = bindings.begin(); it != bindings.end();) {
        if (removals.count(it->first) || pending.count(it->first) || !seen.count(it->first) || !Live(*it->second)) {
            ClearFrame(*it->second);
            ReleaseCapture(*it->second);
            it = bindings.erase(it);
        } else ++it;
    }
    removals.clear();
    for (auto it = windows.begin(); it != windows.end();) {
        if (!seen.count(it->first)) {
            if (it->second.productChild) server->PostToplevelEvent(it->first, ToplevelEventType::Destroyed);
            it = windows.erase(it);
        } else ++it;
    }
    if (wl_ohos_egl_active()) {
        for (const auto& [host, req] : pending) {
            auto b = std::make_unique<Binding>();
            b->source = req.source; b->host = host; b->pid = req.pid; b->sourceKey = req.key;
            b->generation = req.generation; b->xid = req.xid;
            b->x = req.x; b->y = req.y; b->w = req.w; b->h = req.h;
            if (!Live(*b)) continue;
            b->image = OH_ConsumerSurface_Create();
            if (!b->image) continue;
            OH_ConsumerSurface_SetDefaultSize(b->image, b->w, b->h);
            OH_ConsumerSurface_SetDefaultUsage(b->image, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
            auto* producer = OH_NativeImage_AcquireNativeWindow(b->image);
            if (!producer) continue;
            // Bounded queue wait also bounds shutdown if the consumer stops draining.
            OH_NativeWindow_NativeWindowHandleOpt(producer, SET_TIMEOUT, int32_t(100));
            b->renderer = std::make_unique<EglRenderer>(server->GetDesktopCompositor());
            b->renderer->SetIsolatedToplevel(true);
            b->renderer->SetToplevelId(b->source);
            if (!b->renderer->Init(producer, b->w, b->h)) continue;
            bindings[host] = std::move(b);
        }
    }
    pending.clear();
    for (auto& [host, b] : bindings) {
        const bool paused = !windows.at(host).info.mapped;
        if (paused != b->paused) { b->paused = paused; b->renderer->SetPaused(paused); }
        if (paused) continue;
        if (auto* frame = wl_ohos_consumer_buffer_acquire(b->image)) {
            if (wl_ohos_output_client_frame_set(b->xid, frame, 0, b->sourceKey))
                wl_ohos_output_client_frame_region(b->xid, b->x, b->y, b->w, b->h);
            wl_ohos_consumer_buffer_drop(frame);
        }
    }
}
extern "C" void winehua_x11_hosts_shutdown() {
    std::lock_guard<std::mutex> guard(mutex);
    for (const auto& [id, b] : bindings) { ReleaseCapture(*b); ClearFrame(*b); }
    bindings.clear(); pending.clear(); removals.clear();
    for (const auto& [id, win] : windows) if (win.productChild)
        WaylandServer::GetInstance()->PostToplevelEvent(id, ToplevelEventType::Destroyed);
    windows.clear(); grabbedHost = 0; heldButtons.clear();
}
extern "C" bool winehua_x11_external_pointer(uint32_t canvas, int action, double px, double py,
    uint32_t button, double dx, double dy, bool mouse) {
    std::lock_guard<std::mutex> guard(mutex);
    double x, y; Binding* b = Pick(canvas, px, py, x, y);
    if (!b) return false;
    grabbedX = x; grabbedY = y;
    if (action == 1) { grabbedHost = b->host; heldButtons.insert(button); winehua_display_focus_select(b->source, false); }
    InputManager::GetInstance()->SendPointerEvent(b->source, action, x, y, button, dx, dy, mouse, true);
    if (action == 2) { heldButtons.erase(button); if (heldButtons.empty()) grabbedHost = 0; }
    return true;
}
extern "C" bool winehua_x11_external_scroll(uint32_t canvas, int axis, double value, int step, double px, double py) {
    std::lock_guard<std::mutex> guard(mutex);
    double x, y; auto* b = Pick(canvas, px, py, x, y);
    if (!b) return false;
    InputManager::GetInstance()->SendScrollEvent(b->source, axis, value, step, x, y, true);
    return true;
}
