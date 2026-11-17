#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Input focus is window-owned; selecting X11 for Wine must not redirect GNU IME.
void winehua_display_focus_select(uint32_t id, bool x11);
bool winehua_display_focus_is_x11(void);
uint32_t winehua_display_focus_id(void);
uint32_t winehua_x11_desktop_id(void);
bool winehua_x11_is_desktop(uint32_t id);
void winehua_x11_desktop_mapped(int w, int h, uint32_t pid);
void winehua_x11_desktop_reset(void);
void winehua_x11_canvas_size(int w, int h);
void winehua_x11_canvas_dimensions(int *w, int *h);
void winehua_x11_canvas_pointer(int action, double x, double y, uint32_t button);
void winehua_x11_canvas_pointer_raw(int action, double x, double y, uint32_t button, double dx, double dy);
void WineHua_DisplayRoute_StartWithSurface(uint64_t, bool, int, int, bool, bool);
void WineHua_DisplayRoute_DetachCanvas(void);
void WineHua_DisplayRoute_Resize(int, int);
void WineHua_DisplayRoute_CanvasResized(void);
int WineHua_DisplayRoute_Phase(void);
#ifdef __cplusplus
}
#endif
