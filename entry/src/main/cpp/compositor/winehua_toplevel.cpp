/*
 * WineHua toplevel 私有协议服务端 (winehua_toplevel v1)
 *
 * Wine 侧 winewayland.drv 上报模态对话框关系 (xdg-shell 无法表达):
 *   set_modal(surface, owner_surface, modal)
 * 本模块负责:
 *   - global 注册 / bind
 *   - 把关系应用进 ToplevelManager (modalOf_ 组员化 + ToplevelState::ModalOwnerId)
 *   - 通过既有事件通道发 "modal" 事件给 ArkTS (PC 模式子窗口承载依据)
 *
 * 协议回调都在 Wayland 线程执行 (与 xdg_shell.cpp 同装配模式), ToplevelManager
 * 写操作走内部锁 (与 wl_core commit 路径互斥)。
 */
#include <wayland-server-core.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "protocols/winehua-toplevel-server-protocol.h"
#include "wayland_server.h"
#include "compositor/frame/surface_data.h"
#include "compositor/toplevel/toplevel_manager.h"
#include "compositor/toplevel/toplevel_event_bus.h"

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Xdg"
#include <hilog/log.h>

namespace {

ToplevelManager* gTmgr = nullptr;
WaylandServer* gServer = nullptr;

// 校验 + 取 SurfaceData: 参数必须是 wl_surface resource 且 user_data 有效
SurfaceData* sd_from_surface(wl_resource* surfRes) {
    if (!surfRes) return nullptr;
    if (strcmp(wl_resource_get_class(surfRes), wl_surface_interface.name) != 0) return nullptr;
    return static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
}

// 应用模态关系: 组员化/恢复 + 事件 (desktop 模式 compositor 层消费;
// PC 模式 ArkTS 决策子窗口承载, 事件两种模式都发 — ArkTS 端按需消费)
void apply_modal(SurfaceData* sd, uint32_t ownerId, bool modal) {
    if (!sd || !sd->hasToplevel || sd->toplevelId == 0) return;
    int dx = 0, dy = 0, w = 0, h = 0;
    {
        auto lk = gTmgr->Lock();
        gTmgr->SetModalLocked(sd->toplevelId, ownerId, modal);
        // 定位数据: modal 相对 owner 的桌面坐标差 (PC 模式 ArkTS 子窗口
        // 定位 = owner 屏幕位置 + 差*scale); 解除时读不到值保持全 0
        if (modal) {
            const auto* st = gTmgr->FindToplevelLocked(sd->toplevelId);
            const auto* ost = gTmgr->FindToplevelLocked(ownerId);
            if (st) {
                w = st->Width();
                h = st->Height();
                if (st->HasPosition()) {
                    dx = st->X() - (ost && ost->HasPosition() ? ost->X() : 0);
                    dy = st->Y() - (ost && ost->HasPosition() ? ost->Y() : 0);
                }
            }
        }
    }
    gServer->PostToplevelEvent(
        sd->toplevelId, ToplevelEventType::Modal,
        ToplevelEventBus::JsonModal(sd->toplevelId, ownerId, modal ? 1 : 0,
                                    dx, dy, w, h));
    OH_LOG_INFO(LOG_APP, "[XDG] modal tl=%{public}u owner=%{public}u modal=%{public}d"
                " dx=%{public}d dy=%{public}d %{public}dx%{public}d",
                sd->toplevelId, ownerId, modal ? 1 : 0, dx, dy, w, h);
}

// surface 的 toplevelId (来自暂存 pending 时先解析 owner key)
uint32_t resolve_owner_id(SurfaceData* sd, const uint64_t& ownerKey) {
    if (!ownerKey) return 0;
    wl_resource* ownerRes = gTmgr->FindSurfaceResource(ownerKey);
    SurfaceData* ownerSd = ownerRes ? sd_from_surface(ownerRes) : nullptr;
    return (ownerSd && ownerSd->hasToplevel) ? ownerSd->toplevelId : 0;
}

static void wh_set_modal(wl_client*, wl_resource*, wl_resource* surfRes,
                         wl_resource* ownerRes, uint32_t modal) {
    SurfaceData* sd = sd_from_surface(surfRes);
    if (!sd) return;  // 非 winehua 客户端 surface (防御)

    if (modal) {
        SurfaceData* ownerSd = sd_from_surface(ownerRes);
        uint32_t ownerId = 0;
        if (ownerSd) {
            // 正常路径: owner 已建档 toplevel
            if (ownerSd->hasToplevel) {
                ownerId = ownerSd->toplevelId;
            } else {
                // 罕见: owner 的 xdg get_toplevel 还没到 — 暂存, 等建档时应用
                sd->modalPending = true;
                sd->modalOwnerSurfaceKey = ownerSd->surfaceKey;
                OH_LOG_INFO(LOG_APP, "[XDG] modal pending tl-surf key=%{public}llu (owner not toplevel yet)",
                            static_cast<unsigned long long>(ownerSd->surfaceKey));
                return;
            }
        }
        if (ownerId) {
            apply_modal(sd, ownerId, true);
        } else {
            // owner surface 不存在/无 toplevel: 无关系可建, 忽略 (标注限制:
            // MB_TASKMODAL 等无 owner 场景)
            OH_LOG_WARN(LOG_APP, "[XDG] modal set without resolvable owner, ignored tl=%{public}u",
                        sd->toplevelId);
        }
    } else {
        // 解除: 无条件清 (owner 冗余参数忽略)
        apply_modal(sd, 0, false);
    }
}

static const struct winehua_toplevel_interface kInterface = {
    .set_modal = wh_set_modal,
};

static void bind_handler(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &winehua_toplevel_interface,
                                        std::min(version, 1u), id);
    wl_resource_set_implementation(r, &kInterface, nullptr, nullptr);
}

}  // namespace

// get_toplevel 建档后应用暂存的 modal 关系 (xdg_shell.cpp 调用)
extern "C" void WinehuaToplevelApplyPending(wl_resource* surfaceRes) {
    if (!gTmgr || !gServer) return;
    SurfaceData* sd = sd_from_surface(surfaceRes);
    if (!sd || !sd->modalPending) return;
    uint32_t ownerId = resolve_owner_id(sd, sd->modalOwnerSurfaceKey);
    sd->modalPending = false;
    sd->modalOwnerSurfaceKey = 0;
    if (ownerId) {
        apply_modal(sd, ownerId, true);
    } else {
        OH_LOG_WARN(LOG_APP, "[XDG] modal pending dropped (owner unresolvable)");
    }
}

extern "C" void RegisterWinehuaToplevel(wl_display* display) {
    gTmgr = &WaylandServer::GetInstance()->GetToplevelManager();
    gServer = WaylandServer::GetInstance();
    wl_global_create(display, &winehua_toplevel_interface, 1, nullptr, bind_handler);
    OH_LOG_INFO(LOG_APP, "[XDG] winehua_toplevel global registered");
}
