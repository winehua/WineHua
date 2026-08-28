#pragma once
#include <wayland-server-core.h>
#include <cstdint>
#include <vector>
#include "include/xdg-shell-server-protocol.h"

// xdg_toplevel configure 构造单点 (原 8 份手工 wl_array 拷贝收口 — xdg_shell.cpp
// 6 份 + wayland_server.cpp 2 份, docs/COMPOSITOR_REFACTOR_PLAN.md §2.3):
// wl_array_init/add 填充 states → xdg_toplevel_send_configure → wl_array_release
// → xdg_surface_send_configure (serial 取自 toplevel 所属 client 的 display,
// 与各原调用点的 client→display 推导等价)。
//
// 参数化覆盖原各份的差异形态:
// - states 内容与顺序即协议发送顺序, 各调用点保持各自原顺序 (MAXIMIZED/
//   FULLSCREEN/ACTIVATED 的组合与排列是有意的协议语义, 不统一);
// - 宽高各点取值不同 (工作区高 / 输出尺寸 / preMax/preFs 恢复尺寸 / 0,0);
// - 8 份全部带 xdg_surface_send_configure, 故固定包含在 builder 内。
// states 用 vector 而非 initializer_list: 两处调用点 (SetToplevelRestored /
// NotifyToplevelResize) 的状态集是条件组装的, 固定列表表达不了。
inline void XdgConfigureSend(wl_resource* toplevelRes, wl_resource* xdgSurfaceRes,
                             int32_t w, int32_t h, const std::vector<uint32_t>& states)
{
    wl_array arr;
    wl_array_init(&arr);
    for (uint32_t s : states) {
        uint32_t* st = static_cast<uint32_t*>(wl_array_add(&arr, sizeof(uint32_t)));
        *st = s;
    }
    xdg_toplevel_send_configure(toplevelRes, w, h, &arr);
    wl_array_release(&arr);
    wl_display* dpy = wl_client_get_display(wl_resource_get_client(toplevelRes));
    xdg_surface_send_configure(xdgSurfaceRes, wl_display_next_serial(dpy));
}
