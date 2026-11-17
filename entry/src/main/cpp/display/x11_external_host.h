#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct winehua_x11_window {
    uint32_t id, xid, pid;
    uint64_t generation;
    int x, y, w, h;
    bool mapped;
};
// All snapshot/frame functions run on the X11 compositor thread.
size_t wl_ohos_output_snapshot_windows(struct winehua_x11_window*, size_t capacity);
size_t x11_toplevel_snapshot_windows(struct winehua_x11_window*, size_t capacity);
void winehua_x11_hosts_tick(void);
void winehua_x11_hosts_shutdown(void);
bool winehua_x11_host_owns(uint32_t id);
bool winehua_x11_bind_external(uint32_t source, uint32_t host, uint32_t sourcePid,
    uint32_t hostPid, int x, int y, int w, int h);
void winehua_x11_unbind_external(uint32_t host);
bool winehua_x11_external_pointer(uint32_t canvas, int action, double px, double py,
    uint32_t button, double dx, double dy, bool fromMouse);
bool winehua_x11_external_scroll(uint32_t canvas, int axis, double value, int step,
    double px, double py);
// Region is host-client-local. A zero-sized region restores full-window rendering.
int wl_ohos_output_client_frame_region(uint32_t xid, int x, int y, int w, int h);
int x11_toplevel_guest_region(uint32_t xid, int x, int y, int w, int h);
#ifdef __cplusplus
}
#endif
