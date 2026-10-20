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

/* ── M4b-T1: 状态面直译 (bus 既有枚举, 不做策略) ──
 * minimized=false 与 activated 都落 Restored: 语义同为「wine 主动显示
 * 窗口」, ArkTS 侧动作一致 (拉回前台), 幂等无冲突。 */

void x11_toplevel_bridge_post_minimized(uint32_t id, bool minimized)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, minimized ? ToplevelEventType::Minimized
                      : ToplevelEventType::Restored);
}

void x11_toplevel_bridge_post_fullscreen(uint32_t id, bool fullscreen)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, fullscreen ? ToplevelEventType::Fullscreen
                       : ToplevelEventType::Unfullscreen);
}

void x11_toplevel_bridge_post_activated(uint32_t id)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, ToplevelEventType::Restored);
}

void x11_toplevel_bridge_post_modal(uint32_t id, uint32_t owner_id,
                                    int32_t modal, int32_t dx, int32_t dy,
                                    int32_t w, int32_t h)
{
    /* JsonModal(modalId, ownerId, modal, dx, dy, w, h) —— 模板键序与
     * wayland 调用点逐字 (tl=id, owner=owner_id) */
    WaylandServer::GetInstance()->PostToplevelEvent(
        id, ToplevelEventType::Modal,
        ToplevelEventBus::JsonModal(id, owner_id, modal, dx, dy, w, h));
}

/* ── M4c-T1: x11 popup (override_redirect 窗) 四事件 ──
 * 事件 id = owner toplevelId (ArkTS PopupWindowManager 的 parentToplevel,
 * 承载子窗定位/级联销毁都以它为键); popupId 走独立基址空间
 * (X11_POPUP_ID_BASE, x11_toplevel.c), 不占 toplevel id。JSON 形态与
 * wayland 路线 (wl_core.cpp UpdateSubsurfaceOnCommit) 逐字一致。 */

void x11_toplevel_bridge_post_popup_show(uint32_t parent_id, uint32_t popup_id,
                                         int32_t x, int32_t y,
                                         int32_t w, int32_t h, int32_t argb01)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        parent_id, ToplevelEventType::PopupShow,
        ToplevelEventBus::JsonPopupShow(popup_id, x, y, w, h, argb01));
}

void x11_toplevel_bridge_post_popup_move(uint32_t parent_id, uint32_t popup_id,
                                         int32_t x, int32_t y)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        parent_id, ToplevelEventType::PopupMove,
        ToplevelEventBus::JsonPopupMove(popup_id, x, y));
}

void x11_toplevel_bridge_post_popup_resize(uint32_t parent_id, uint32_t popup_id,
                                           int32_t w, int32_t h)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        parent_id, ToplevelEventType::PopupResize,
        ToplevelEventBus::JsonPopupResize(popup_id, w, h));
}

void x11_toplevel_bridge_post_popup_hide(uint32_t parent_id, uint32_t popup_id)
{
    WaylandServer::GetInstance()->PostToplevelEvent(
        parent_id, ToplevelEventType::PopupHide,
        ToplevelEventBus::JsonPopupHide(popup_id));
}

uint32_t x11_toplevel_bridge_allocate_id(void)
{
    return WaylandServer::GetInstance()->GetToplevelManager().AllocateToplevelId();
}

}  /* extern "C" */
