#include "display_session.h"
#include "display_input.h"
#include "compositor/wayland_server.h"
#include <atomic>
namespace {
std::atomic<uint32_t> desktopId{0};
std::atomic<uint64_t> focus{0};
std::atomic<int> canvasW{1}, canvasH{1};
}
extern "C" void winehua_display_focus_select(uint32_t id, bool x11) {
    focus.store(uint64_t(id) | (x11 ? (uint64_t(1) << 32) : 0));
}
extern "C" bool winehua_display_focus_is_x11() { return (focus.load() >> 32) != 0; }
extern "C" uint32_t winehua_display_focus_id() { return uint32_t(focus.load()); }
extern "C" uint32_t winehua_x11_desktop_id() { return desktopId.load(); }
extern "C" bool winehua_x11_is_desktop(uint32_t id) { return id && id == desktopId.load(); }
extern "C" void winehua_x11_desktop_mapped(int w, int h, uint32_t pid) {
    if (desktopId.load()) return;
    auto *server = WaylandServer::GetInstance();
    const auto id = server->GetToplevelManager().AllocateToplevelId();
    desktopId.store(id);
    server->PostToplevelEvent(id, ToplevelEventType::Created, ToplevelEventBus::JsonCreated(w, h, pid));
    server->PostToplevelEvent(id, ToplevelEventType::DesktopRoot);
}
extern "C" void winehua_x11_desktop_reset() {
    uint64_t old = focus.load();
    while ((old >> 32) && !focus.compare_exchange_weak(old, 0)) {}
    const auto id = desktopId.exchange(0);
    if (id) WaylandServer::GetInstance()->PostToplevelEvent(id, ToplevelEventType::Destroyed);
}
extern "C" void winehua_x11_canvas_size(int w, int h) {
    if (w > 0 && h > 0) { canvasW.store(w); canvasH.store(h); }
}
extern "C" void winehua_x11_canvas_pointer(int action, double x, double y, uint32_t button) {
    wl_ohos_input_post_motion(x / canvasW.load(), y / canvasH.load(), action == 5 ? 2 : 1);
    if (action == 1 || action == 2) wl_ohos_input_post_button(button, action == 1);
}

extern "C" void winehua_x11_canvas_dimensions(int* w, int* h) {
    if (w) *w = canvasW.load(); if (h) *h = canvasH.load();
}

extern "C" void winehua_x11_canvas_pointer_raw(int action, double x, double y, uint32_t button, double dx, double dy) {
    wl_ohos_input_post_motion_raw(x / canvasW.load(), y / canvasH.load(), action == 5 ? 2 : 1,
                                action == 3 ? dx : 0, action == 3 ? dy : 0);
    if (action == 1 || action == 2) wl_ohos_input_post_button(button, action == 1);
}
