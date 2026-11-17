#pragma once
#include "wayland-server-core.h"
// Mock only the protocol I/O boundary; the production injector controls
// ownership, sequencing, client selection and committed focus.
void HostTestProtocolEvent(wl_resource*, const char*, wl_resource*);
void HostTestAxisEvent(wl_resource*, uint32_t, int32_t, bool);
void HostTestMotionEvent(wl_resource*, wl_fixed_t, wl_fixed_t);
constexpr uint32_t WL_POINTER_AXIS_VERTICAL_SCROLL = 0;
constexpr uint32_t WL_POINTER_AXIS_HORIZONTAL_SCROLL = 1;
constexpr int WL_POINTER_AXIS_DISCRETE_SINCE_VERSION = 5;
constexpr uint32_t WL_POINTER_BUTTON_STATE_RELEASED = 0;
constexpr uint32_t WL_POINTER_BUTTON_STATE_PRESSED = 1;
constexpr uint32_t WL_KEYBOARD_KEY_STATE_RELEASED = 0;
constexpr uint32_t WL_KEYBOARD_KEY_STATE_PRESSED = 1;
inline void wl_pointer_send_enter(wl_resource* r, uint32_t, wl_resource* s, wl_fixed_t, wl_fixed_t) {
    HostTestProtocolEvent(r, "pointer.enter", s);
}
inline void wl_pointer_send_leave(wl_resource* r, uint32_t, wl_resource* s) {
    HostTestProtocolEvent(r, "pointer.leave", s);
}
inline void wl_pointer_send_frame(wl_resource*) {}
inline void wl_pointer_send_motion(wl_resource* r, uint32_t, wl_fixed_t x, wl_fixed_t y) { HostTestMotionEvent(r,x,y); }
inline void wl_pointer_send_button(wl_resource* r, uint32_t, uint32_t, uint32_t, uint32_t) {
    HostTestProtocolEvent(r, "pointer.button", nullptr);
}
inline void wl_pointer_send_axis(wl_resource* r, uint32_t, uint32_t a, wl_fixed_t v) { HostTestAxisEvent(r,a,v,false); }
inline void wl_pointer_send_axis_discrete(wl_resource* r, uint32_t a, int32_t v) { HostTestAxisEvent(r,a,v,true); }
inline void wl_keyboard_send_enter(wl_resource* r, uint32_t, wl_resource* s, wl_array*) {
    HostTestProtocolEvent(r, "keyboard.enter", s);
}
inline void wl_keyboard_send_leave(wl_resource* r, uint32_t, wl_resource* s) {
    HostTestProtocolEvent(r, "keyboard.leave", s);
}
inline void wl_keyboard_send_key(wl_resource* r, uint32_t, uint32_t, uint32_t, uint32_t) {
    HostTestProtocolEvent(r, "keyboard.key", nullptr);
}
inline void wl_keyboard_send_modifiers(wl_resource*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
