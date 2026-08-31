#pragma once
#include <wayland-server-core.h>

// Toplevel 独立的 user_data，不共享 XdgSurface*
// 解决 wl_client_destroy 时 xs_resource_destroy 先释放 XdgSurface，
// 然后 tl_resource_destroy 访问野指针的问题
struct ToplevelData {
    uint32_t toplevelId = 0;
    wl_resource* xdgSurface = nullptr;  // 回指针，供 tl_set_title 等操作
};

struct XdgSurface {
    wl_resource* wlSurface = nullptr;
    wl_resource* xdgSurface = nullptr;
    wl_resource* xdgToplevel = nullptr;
    uint32_t toplevelId = 0;
};

// 注册 xdg_wm_base global 到 display
extern "C" void RegisterXdgShell(wl_display* display);
