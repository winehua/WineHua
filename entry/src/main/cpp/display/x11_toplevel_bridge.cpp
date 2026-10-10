/* M4a: x11_toplevel.c (C, xs 字段访问) → toplevel_event_bus (C++) 的事件
 * 投递桥。JSON 构造单点 (ToplevelEventBus::Json*) + ToplevelEventName 红线
 * + Post 全在此 —— C 侧只透传语义, 不碰事件名字符串。 */
#include <cstdint>

#include "compositor/toplevel/toplevel_event_bus.h"
#include "compositor/wayland_server.h"

extern "C" {

void x11_toplevel_bridge_post_created(uint32_t id, int32_t w, int32_t h,
                                      int32_t x, int32_t y)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, ToplevelEventType::Created,
        ToplevelEventBus::JsonCreatedAt(w, h, x, y));
}

void x11_toplevel_bridge_post_title(uint32_t id, const char *title)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, ToplevelEventType::Title, ToplevelEventBus::JsonTitle(title));
}

void x11_toplevel_bridge_post_resize(uint32_t id, int32_t w, int32_t h)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, ToplevelEventType::Resize,
        ToplevelEventBus::JsonResize(w, h));
}

void x11_toplevel_bridge_post_destroyed(uint32_t id)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, ToplevelEventType::Destroyed);
}

uint32_t x11_toplevel_bridge_allocate_id(void)
{
    return WaylandServer::GetInstance()->GetToplevelManager().AllocateToplevelId();
}

}  /* extern "C" */
