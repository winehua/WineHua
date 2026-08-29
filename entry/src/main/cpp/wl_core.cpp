// wl_core.cpp — Wayland 核心协议实现。
// wl_compositor / wl_surface / wl_region / wl_subcompositor / wl_subsurface /
// wp_viewporter / wp_viewport / wl_output 的接口表与回调, 以及 global 注册。
//
// 从 wayland_server.cpp 剥离 (Phase 3, 纯搬移、零行为变化):
// 回调均为 WaylandServer 的 static 成员, 经 GetInstance() 访问共享状态
// (ToplevelManager / DesktopCompositor / InputManager 等)。

#include "wayland_server.h"
#include "seat.h"
#include "input_manager.h"
#include "plugin_manager.h"
#include "pointer_extras.h"
#include "text_input.h"
#include "compositor/compositor_utils.h"
#include "compositor/compositor_constants.h"
#include "compositor/geometry.h"
#include "compositor/shm_frame_source.h"  // SHM 拷贝/缩放纯函数 (重构第 5A1 步迁出)
#include "include/viewporter-server-protocol.h"
#include "perf_utils.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cmath>
#include <cstdio>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"
#include <hilog/log.h>

// -- wl_surface 接口实现表 --
static const struct wl_surface_interface kSurfaceImpl = {
    .destroy           = WaylandServer::surface_destroy,
    .attach            = WaylandServer::surface_attach,
    .damage            = WaylandServer::surface_damage,
    .frame             = WaylandServer::surface_frame,
    .set_opaque_region = WaylandServer::surface_set_opaque_region,
    .set_input_region  = WaylandServer::surface_set_input_region,
    .commit            = WaylandServer::surface_commit,
    .set_buffer_transform = WaylandServer::surface_set_buffer_transform,
    .set_buffer_scale  = WaylandServer::surface_set_buffer_scale,
    .damage_buffer     = WaylandServer::surface_damage_buffer,
    .offset            = WaylandServer::surface_offset,
};

static const struct wl_region_interface kRegionImpl = {
    .destroy  = WaylandServer::region_destroy,
    .add      = WaylandServer::region_add,
    .subtract = WaylandServer::region_subtract,
};

static const struct wl_subcompositor_interface kSubcompositorImpl = {
    .destroy        = WaylandServer::subcompositor_destroy,
    .get_subsurface = WaylandServer::subcompositor_get_subsurface,
};

static const struct wl_subsurface_interface kSubsurfaceImpl = {
    .destroy       = WaylandServer::subsurface_destroy,
    .set_position  = WaylandServer::subsurface_set_position,
    .place_above   = WaylandServer::subsurface_place_above,
    .place_below   = WaylandServer::subsurface_place_below,
    .set_sync      = WaylandServer::subsurface_set_sync,
    .set_desync    = WaylandServer::subsurface_set_desync,
};

static const struct wp_viewporter_interface kViewporterImpl = {
    .destroy      = WaylandServer::viewporter_destroy,
    .get_viewport = WaylandServer::viewporter_get_viewport,
};

static const struct wp_viewport_interface kViewportImpl = {
    .destroy        = WaylandServer::viewport_destroy,
    .set_source     = WaylandServer::viewport_set_source,
    .set_destination = WaylandServer::viewport_set_destination,
};

static const struct wl_output_interface kOutputImpl = {
    .release = WaylandServer::output_release,
};

static const struct wl_compositor_interface kCompositorImpl = {
    .create_surface = WaylandServer::compositor_create_surface,
    .create_region  = WaylandServer::compositor_create_region,
};

// -- compositor 实现 --
void WaylandServer::compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(res, &kCompositorImpl, data, nullptr);
}

void WaylandServer::compositor_create_surface(wl_client* client, wl_resource* compRes, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] client created wl_surface id=%{public}u", id);
    auto* sd = new SurfaceData();
    wl_resource* surfRes = wl_resource_create(client, &wl_surface_interface,
                                              wl_resource_get_version(compRes), id);
    sd->surface = surfRes;
    sd->clientPid = GetWaylandClientPid(client);
    sd->protocolId = id;
    sd->surfaceKey = MakeSurfaceKey(sd->clientPid, id);
    OH_LOG_INFO(LOG_APP,
                "[MW-ZC] surface created pid=%{public}u surface=%{public}u key=%{public}llu resource=%{public}p",
                sd->clientPid, sd->protocolId,
                static_cast<unsigned long long>(sd->surfaceKey), surfRes);
    wl_resource_set_implementation(surfRes, &kSurfaceImpl, sd, [](wl_resource* r) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(r));
        auto* self = GetInstance();
        uint32_t removedPopup = 0, popupParent = 0;
        {
            auto lk = self->toplevelMgr_.Lock();
            const uint64_t surfaceKey = sd ? sd->surfaceKey : 0;
            self->toplevelMgr_.UnregisterSurfaceResource(surfaceKey);
            self->desktopCompositor_.RemoveZeroCopyKeyLocked(surfaceKey);
            self->desktopCompositor_.RemoveSubsurfaceLayer(r);
            // PC popup 记录一并清除 (client 断开时 libwayland 走此路径)
            if (sd) {
                popupParent = self->toplevelMgr_.RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            }
            self->MarkDesktopRootDirtyLocked();
        }
        // 无条件重置输入焦点: 任何 surface (含 desktop 菜单 subsurface) 销毁时
        // 都可能是当前 pointer/keyboard 焦点 — 焦点悬垂后下一次 leave 会引用
        // 已复用的对象 id, client 报 "invalid object ... leave(uo)" 并断开。
        // 内部按指针比较, 非焦点 surface 是 no-op
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        if (sd && sd->hasToplevel) {
            {
                self->toplevelMgr_.UnmapToplevelSurface(sd->toplevelId);
            }
            self->FireToplevelEvent(sd->toplevelId, "destroyed");
        }
        if (removedPopup) {
            char json[64];
            snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
            self->FireToplevelEvent(popupParent, "popup_hide", json);
        }
        delete sd;
    });
    {
        auto* self = static_cast<WaylandServer*>(wl_resource_get_user_data(compRes));
        auto lk = self->toplevelMgr_.Lock();
        self->toplevelMgr_.RegisterSurfaceResource(sd->surfaceKey, surfRes);
    }
}

void WaylandServer::compositor_create_region(wl_client* client, wl_resource* compRes, uint32_t id) {
    int* rectCount = new int(0);
    wl_resource* res = wl_resource_create(client, &wl_region_interface,
                                          wl_resource_get_version(compRes), id);
    wl_resource_set_implementation(res, &kRegionImpl, rectCount, [](wl_resource* r) {
        delete static_cast<int*>(wl_resource_get_user_data(r));
    });
}

// -- subcompositor 实现 --
void WaylandServer::subcompositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wl_subcompositor bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wl_subcompositor_interface, version, id);
    wl_resource_set_implementation(res, &kSubcompositorImpl, data, nullptr);
}

void WaylandServer::subcompositor_get_subsurface(wl_client* client, wl_resource*,
                                                  uint32_t id, wl_resource* surface,
                                                  wl_resource* parent) {
    // 追踪 subsurface 父子关系:
    // 子 surface → 记录父 surface 指针, 标记 isSubsurface
    // wl_subsurface 的 user_data 存子 surface, 供 set_position 查找
    auto* childSd = static_cast<SurfaceData*>(wl_resource_get_user_data(surface));
    if (childSd) {
        childSd->parentSurface = parent;
        childSd->isSubsurface = true;
        // CommittedSurface.role 即时同步 (重构第 5A2 步): role 随协议角色
        // 设置点更新, 不再由 commit 时从布尔分流; 判定单点 = RoleFor
        childSd->committed.role = RoleFor(childSd->hasToplevel, childSd->isSubsurface);
        OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] subsurface created: child=%{public}p parent=%{public}p",
                    surface, parent);
    }

    // wl_subsurface resource 的 user_data = 子 surface (供 set_position 查找 SurfaceData)
    wl_resource* ss = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(ss, &kSubsurfaceImpl, surface, nullptr);
}

void WaylandServer::subsurface_set_position(wl_client*, wl_resource* ssRes,
                                             int32_t x, int32_t y) {
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(childSurf));
    if (!sd) return;
    if (sd->subsurfaceX == x && sd->subsurfaceY == y) return;
    sd->subsurfaceX = x;
    sd->subsurfaceY = y;
    auto* self = GetInstance();
    {
        auto lk = self->toplevelMgr_.Lock();
        self->desktopCompositor_.UpdateSubsurfaceLayerLocalPosition(childSurf, x, y);
        self->MarkDesktopRootDirtyLocked();
    }
    // PC 模式: 更新已登记 popup 的偏移, 通知 ArkTS 移动子窗口。
    // 父几何读点 (重构第 5A2 步): 旧读 parentSd->geoX/geoY (即时窗口几何值),
    // 新读 parentSd->committed.contentRect.x/y — 同一写入点 (xs_set_window_geometry
    // 直写快照) 的同步表达式, 逐点等价; 偏移公式语义不变 (相对父内容原点)。
    uint32_t movePopupId = 0, moveParent = 0;
    int32_t moveOffX = 0, moveOffY = 0;
    {
        auto lk = self->toplevelMgr_.Lock();
        uint32_t pid = self->toplevelMgr_.FindPopupBySurfaceKey(sd->surfaceKey);
        if (auto* rec = self->toplevelMgr_.FindPopup(pid)) {
            int32_t parentContentX = 0, parentContentY = 0;
            if (sd->parentSurface) {
                auto* parentSd = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
                if (parentSd) {
                    parentContentX = parentSd->committed.contentRect.x;
                    parentContentY = parentSd->committed.contentRect.y;
                }
            }
            rec->offX = x - parentContentX;
            rec->offY = y - parentContentY;
            movePopupId = rec->popupId;
            moveParent = rec->parentToplevel;
            moveOffX = rec->offX;
            moveOffY = rec->offY;
        }
    }
    if (movePopupId) {
        char json[128];
        snprintf(json, sizeof(json), "{\"popupId\":%u,\"x\":%d,\"y\":%d}",
                 movePopupId, moveOffX, moveOffY);
        self->FireToplevelEvent(moveParent, "popup_move", json);
    }
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] set_position: child=%{public}p parent=%{public}p pos=(%{public}d,%{public}d)",
                childSurf, sd->parentSurface, x, y);
}

void WaylandServer::subsurface_place_above(wl_client*, wl_resource* ssRes, wl_resource* sibling) {
    /*
     * 风险标注 (P2): place_above/place_below 只维护 desktop 模式的
     * desktopCompositor_.subsurfaceLayers() 顺序。PC 模式的 popup 是独立 OHOS 子窗口,
     * z-order 由窗口系统按创建顺序决定, 此处的重排不会映射到子窗口。
     * 桌面语义上 popup 恒在父窗口之上 (OHOS 子窗口天然满足),
     * 多 popup (子菜单链) 交叠顺序极端情况下可能与客户端预期不符。
     */
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf || !sibling) return;
    auto* self = GetInstance();
    auto lk = self->toplevelMgr_.Lock();
    const bool changed =
        self->desktopCompositor_.ReorderSubsurfaceLayerAbove(childSurf, sibling);
    if (changed && self->Policy().RootCompositing()) self->MarkDesktopRootDirtyLocked();
    if (changed)
        OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] place_above child=%{public}p above sibling=%{public}p",
                    childSurf, sibling);
}

void WaylandServer::subsurface_place_below(wl_client*, wl_resource* ssRes, wl_resource* sibling) {
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf || !sibling) return;
    auto* self = GetInstance();
    auto lk = self->toplevelMgr_.Lock();
    const bool changed =
        self->desktopCompositor_.ReorderSubsurfaceLayerBelow(childSurf, sibling);
    if (changed && self->Policy().RootCompositing()) self->MarkDesktopRootDirtyLocked();
    if (changed)
        OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] place_below child=%{public}p below sibling=%{public}p",
                    childSurf, sibling);
}

void WaylandServer::subsurface_destroy(wl_client*, wl_resource* r) {
    // role 移除: surface 变回普通 surface。按 unmap 处理:
    // 清 desktop layer / PC popup 记录, 通知 ArkTS 销毁 popup 子窗口。
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(r));
    if (childSurf) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(childSurf));
        if (sd) {
            sd->isSubsurface = false;
            sd->parentSurface = nullptr;
            // CommittedSurface.role 即时同步 (重构第 5A2 步): role 移除后
            // surface 变回普通面 (旧代码只清 isSubsurface, 语义对等)
            sd->committed.role = RoleFor(sd->hasToplevel, sd->isSubsurface);
            auto* self = GetInstance();
            uint32_t removedPopup = 0, popupParent = 0;
            {
                auto lk = self->toplevelMgr_.Lock();
                self->desktopCompositor_.RemoveSubsurfaceLayer(childSurf);
                popupParent = self->toplevelMgr_.RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            }
            if (removedPopup) {
                char json[64];
                snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
                self->FireToplevelEvent(popupParent, "popup_hide", json);
            }
        }
    }
    wl_resource_destroy(r);
}

// -- viewporter 实现 --
void WaylandServer::viewporter_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wp_viewporter bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wp_viewporter_interface, version, id);
    wl_resource_set_implementation(res, &kViewporterImpl, data, nullptr);
}

void WaylandServer::viewporter_get_viewport(wl_client* client, wl_resource*,
                                             uint32_t id, wl_resource* surface) {
    wl_resource* vp = wl_resource_create(client, &wp_viewport_interface, 1, id);
    // 把 surface resource 存为 viewport 的 user_data,
    // 这样 viewport_set_destination 就能通过 surface 找到 SurfaceData
    wl_resource_set_implementation(vp, &kViewportImpl, surface, nullptr);
}

void WaylandServer::viewport_set_source(wl_client*, wl_resource* vpRes,
                                        wl_fixed_t fx, wl_fixed_t fy, wl_fixed_t fw, wl_fixed_t fh) {
    auto* surf = static_cast<wl_resource*>(wl_resource_get_user_data(vpRes));
    if (!surf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
    if (!sd) return;
    if (wl_fixed_to_int(fw) == -1 && wl_fixed_to_int(fh) == -1) {
        // unset: 恢复全 buffer (注意参数是 wl_fixed_t, unset 编码为 wl_fixed_from_int(-1))
        sd->vpSrcX = 0;
        sd->vpSrcY = 0;
        sd->vpSrcW = -1;
        sd->vpSrcH = -1;
        return;
    }
    sd->vpSrcX = wl_fixed_to_int(fx);
    sd->vpSrcY = wl_fixed_to_int(fy);
    sd->vpSrcW = wl_fixed_to_int(fw);
    sd->vpSrcH = wl_fixed_to_int(fh);
    OH_LOG_INFO(LOG_APP, "[MW-VP] set_source surf=%{public}p tl=%{public}u src=(%{public}d,%{public}d %{public}dx%{public}d)",
                surf, sd->toplevelId, sd->vpSrcX, sd->vpSrcY, sd->vpSrcW, sd->vpSrcH);
}

void WaylandServer::viewport_set_destination(wl_client*, wl_resource* vpRes, int32_t w, int32_t h) {
    auto* surf = static_cast<wl_resource*>(wl_resource_get_user_data(vpRes));
    if (!surf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
    if (!sd) return;
    sd->vpDstW = w;
    sd->vpDstH = h;
}

// -- output 实现 --
void WaylandServer::output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wl_output bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wl_output_interface, version, id);
    wl_resource_set_implementation(res, &kOutputImpl, data, nullptr);
    auto* self = static_cast<WaylandServer*>(data);
    // 发送虚拟显示器信息 (尺寸由 ArkTS 初始化时设置)。
    // Wine 侧消费方式 (winewayland.drv):
    // - mode 尺寸/refresh → 显示模式列表, 应用经 ChangeDisplaySettings 见到;
    //   桌面模式下 explorer 按它铺 desktop 窗口, 全屏游戏按它选渲染分辨率
    // - geometry 物理尺寸 → 推算 DPI (按 96DPI 基准从默认分辨率折算, 见常量)
    // - scale 恒 1: Wine 逻辑像素 = 合成像素, 缩放由 ArkTS 侧 effectiveScale 承担
    int32_t pw = self->outputW_, ph = self->outputH_;
    // 物理尺寸按默认分辨率折算 (1280x720 → 340x190mm ≈ 96DPI)
    int32_t physW = pw * compositor_consts::kOutputPhysWidthMm / compositor_consts::kDefaultOutputWidth;
    int32_t physH = ph * compositor_consts::kOutputPhysHeightMm / compositor_consts::kDefaultOutputHeight;
    OH_LOG_INFO(LOG_APP, "[WL] output_bind: %{public}dx%{public}d (phys %{public}dx%{public}d)",
                pw, ph, physW, physH);
    wl_output_send_geometry(res, 0, 0, physW, physH,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "Wine", "Virtual",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        pw, ph, compositor_consts::kOutputRefreshMillihertz);
    if (version >= 2) wl_output_send_scale(res, 1);
    if (version >= 4) {
        wl_output_send_name(res, "Wine-Virtual-0");
        wl_output_send_description(res, "Virtual output for Wine Wayland driver");
    }
    wl_output_send_done(res);
}

// -- surface 实现 --
void WaylandServer::surface_destroy(wl_client*, wl_resource* r) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(r));
    OH_LOG_INFO(LOG_APP, "[MW-Life] surface_destroy surf=%{public}p hasToplevel=%{public}d toplevelId=%{public}u isSubsurface=%{public}d",
                r, sd ? sd->hasToplevel : 0, sd ? sd->toplevelId : 0, sd ? sd->isSubsurface : 0);
    if (sd && sd->hasToplevel) {
        // 清理 surface 映射
        auto* self = GetInstance();
        self->toplevelMgr_.UnmapToplevelSurface(sd->toplevelId);
        // 清理 toplevel 像素数据 + 标记 root dirty
        // (root 被销毁时 OnToplevelDestroyed 内部已复位 desktopRootToplevelId_)
        self->OnToplevelDestroyed(sd->toplevelId);
        // 重置 InputManager 焦点: 防止后续 Inject*Leave 引用已销毁的 surface
        // (否则 Wine 收到 invalid object 协议错误 → 断开连接)
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        self->FireToplevelEvent(sd->toplevelId, "destroyed");
    }
    // subsurface 销毁: 清除 layer + 标记 root dirty 触发重绘 (移除残留像素)
    if (sd && sd->isSubsurface) {
        auto* self = GetInstance();
        uint32_t removedPopup = 0, popupParent = 0;
        {
            auto lk = self->toplevelMgr_.Lock();
            self->desktopCompositor_.RemoveSubsurfaceLayer(r);
            // PC popup 记录一并清除
            popupParent = self->toplevelMgr_.RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            if (self->Policy().RootCompositing()) self->MarkDesktopRootDirtyLocked();
        }
        if (removedPopup) {
            // 防止 pointer focus 悬在已销毁的 popup surface 上 (协议错误会断开 Wine)
            InputManager::GetInstance()->OnSurfaceDestroyed(r);
            char json[64];
            snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
            self->FireToplevelEvent(popupParent, "popup_hide", json);
        }
    }
    wl_resource_destroy(r);
}

void WaylandServer::surface_attach(wl_client*, wl_resource* surfRes, wl_resource* buffer, int32_t, int32_t) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    sd->pendingBuffer = buffer;
    // 新 buffer → 重置 damage
    sd->damageX = 0; sd->damageY = 0;
    sd->damageW = 0; sd->damageH = 0;
}

void WaylandServer::surface_damage(wl_client*, wl_resource* surfRes,
                                    int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    if (!sd) return;
    // 累积 damage 包围盒 (union)
    if (sd->damageW == 0 || sd->damageH == 0) {
        sd->damageX = x; sd->damageY = y;
        sd->damageW = w; sd->damageH = h;
    } else {
        int32_t rx = std::min(sd->damageX, x);
        int32_t ry = std::min(sd->damageY, y);
        int32_t rr = std::max(sd->damageX + sd->damageW, x + w);
        int32_t rb = std::max(sd->damageY + sd->damageH, y + h);
        sd->damageX = rx; sd->damageY = ry;
        sd->damageW = rr - rx; sd->damageH = rb - ry;
    }
}

void WaylandServer::surface_set_input_region(wl_client*, wl_resource* surfRes, wl_resource* region) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    if (!sd) return;
    if (region) {
        int* count = static_cast<int*>(wl_resource_get_user_data(region));
        sd->inputRegionEmpty = (count && *count == 0);
    } else {
        sd->inputRegionEmpty = false;  // NULL region = 整面接受输入
    }
}

void WaylandServer::surface_frame(wl_client* client, wl_resource* surfRes, uint32_t cbId) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    wl_resource* cbRes = wl_resource_create(client, &wl_callback_interface, 1, cbId);
    sd->frameCallbacks.push_back(cbRes);
}

// ========================================================================
//  wl_surface.commit — pending 状态应用为当前帧 (核心热点路径)
//
//  协议语义: wl_surface 是双缓冲, attach/damage/set_* 只改 pending 状态,
//  commit 时一次性生效。本函数把 shm 像素拷出并按 surface 角色分发:
//  toplevel 帧存档 / desktop subsurface 存 layer / PC subsurface 登记 popup,
//  最后 release buffer 并回发 frame callback (客户端据此做帧节流)。
//  分段实现, 每段头注释注明协议语义; 共享的 shm 帧信息经 ShmCommitInfo 传递。
// ========================================================================

// 注: SHM 拷贝/缩放纯函数 (CopyShmContentTight/CopyToplevelContent/CopyShmBufferTight)
// 已迁至 compositor/shm_frame_source.{h,cpp} (重构第 5A1 步, 行为平价), 调用点见下方。

// NULL buffer commit: surface 无内容 (wl_surface.attach(NULL)+commit 即 unmap)。
// 清除对应 desktop subsurface layer / PC popup 记录并通知 ArkTS。
// 返回 true = 本次 commit 已处理完 (无 buffer, 无后续流程)。
bool WaylandServer::HandleNullBufferCommit(SurfaceData* sd, wl_resource* surfRes) {
    if (sd->pendingBuffer) return false;
    if (sd->isSubsurface) {
        uint32_t removedPopup = 0, popupParent = 0;
        {
            auto lk = toplevelMgr_.Lock();
            if (desktopCompositor_.RemoveSubsurfaceLayer(surfRes)) {
                OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] NULL buffer commit → removed layer");
                if (Policy().RootCompositing()) MarkDesktopRootDirtyLocked();
            }
            // PC popup: unmap (菜单关闭) → 销毁 ArkTS 子窗口
            popupParent = toplevelMgr_.RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
        }
        if (removedPopup) {
            OH_LOG_INFO(LOG_APP, "[MW-POPUP] hide popup=#%{public}u parent=#%{public}u (NULL buffer commit)",
                        removedPopup, popupParent);
            char json[64];
            snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
            FireToplevelEvent(popupParent, "popup_hide", json);
        }
    }
    return true;
}

// 读取 pending wl_shm buffer 并 begin_access。
// 协议: commit 期间必须 begin_access/end_access 包住像素读, 防客户端并发改写。
// 返回 false = 非 shm buffer (如 dmabuf), 跳过像素路径直接 FinishCommit。
bool WaylandServer::BeginShmAccess(SurfaceData* sd, ShmCommitInfo& fi) {
    fi.shm = wl_shm_buffer_get(sd->pendingBuffer);
    if (!fi.shm) return false;
    fi.bufW = wl_shm_buffer_get_width(fi.shm);
    fi.bufH = wl_shm_buffer_get_height(fi.shm);
    fi.stride = wl_shm_buffer_get_stride(fi.shm);
    // WL_SHM_FORMAT_ARGB8888=0 (有意义 alpha, layered/shaped 窗口), XRGB8888=1 (无 alpha)
    fi.shmFormat = wl_shm_buffer_get_format(fi.shm);
    wl_shm_buffer_begin_access(fi.shm);
    fi.src = static_cast<const uint8_t*>(wl_shm_buffer_get_data(fi.shm));
    return true;
}

// 计算实际内容区: 优先 xdg_surface window_geometry (协议: geometry 是
// buffer 内的"可见内容"矩形), 否则全 buffer。toplevel 的 geoX/geoY 在桌面
// 模式另有含义 (虚拟桌面屏幕位置), subsurface 则是相对父 surface 的偏移。
// 几何计算段已收口到 ShmFrameSource::ComputeContentAreaGeometry (重构第
// 5A1 步, SurfaceData 字段值语义参数化, 逻辑逐字搬移, 行为平价); 本函数
// 保留 hilog 日志 (MW-GEO/MW-STRIDE), 条件/文本/顺序与旧实现逐字一致。
// 三义分流显式化 (重构第 5A2 步): 旧代码把 sd->geoX/geoY (三义字段, PLAN
// §2.4) 原样喂给纯函数, 由函数内 hasToplevel 猜义; 现按命名字段取义 —
// 角色 = committed.role (显式枚举), 几何值 = contentRect.x/y (写点直写的
// 即时值; toplevel 的"屏幕位置"义在纯函数内转成 fi.screenX/Y 后另存
// committed.screenX/Y, 本处不再直接读)。喂入的值与旧 geo 字段逐点相同。
void WaylandServer::ComputeContentArea(SurfaceData* sd, ShmCommitInfo& fi) {
    const auto& c = sd->committed;
    ComputeContentAreaGeometry(fi, c.hasWindowGeometry, c.contentRect.w, c.contentRect.h,
                               c.role == CommittedSurface::Role::Toplevel,
                               c.contentRect.x, c.contentRect.y);
    if (c.hasWindowGeometry && c.contentRect.w > 0 && c.contentRect.h > 0) {
        OH_LOG_INFO(LOG_APP, "[MW-GEO] using window_geometry: src=%{public}dx%{public}d geo=(%{public}d,%{public}d %{public}dx%{public}d) screen=(%{public}d,%{public}d) vpSrc=(%{public}d,%{public}d %{public}dx%{public}d) vpDst=%{public}dx%{public}d",
                    fi.bufW, fi.bufH, fi.contentOffX, fi.contentOffY, fi.contentW, fi.contentH,
                    fi.screenX, fi.screenY,
                    sd->vpSrcX, sd->vpSrcY, sd->vpSrcW, sd->vpSrcH, sd->vpDstW, sd->vpDstH);
    }

    // stride 与 w*4 不一致是异常 (wl_shm 允许 padding), 仅告警不处理
    if (fi.stride != fi.bufW * 4) {
        OH_LOG_WARN(LOG_APP, "[MW-STRIDE] stride mismatch! w=%{public}d h=%{public}d stride=%{public}d rowBytes=%{public}d",
                    fi.bufW, fi.bufH, fi.stride, fi.bufW * 4);
    }
}

// CommittedSurface 快照产出 (重构第 5A2 步·2/2, 行为平价):
// commit 管线把同一数据填入命名快照 (screenPos/parentOffset/frame) — 后续
// 阶段语义段各归其主时的消费载体。值均直接拷贝自同源字段/fi:
//   - role/hasWindowGeometry/contentRect: 已即时直写 (role 随协议角色设置点
//     同步更新, 几何写点 xdg_shell xs_set_window_geometry 直写快照), 本函数
//     不再填充 — 与旧"写 geo 字段 → commit 时读 geo 字段"值流逐点等价
//   - screenPos: ComputeContentArea 计算出的 ShmCommitInfo::screenX/Y
//     (语义 = 旧 geoX/geoY 的"虚拟桌面屏幕位置"义)
//   - parentOffset/frame: SurfaceData 同值字段拷贝
// 调用点: surface_commit (BeginShmAccess 成功后、角色分发前), 单次 commit
// 一份快照; NULL buffer commit (HandleNullBufferCommit 提前 return) 不更新
// — 与旧字段行为一致。锁: 无 (wl 事件循环线程, 与 SurfaceData 同域)。
void WaylandServer::BuildCommittedSurface(SurfaceData* sd, ShmCommitInfo& fi) {
    auto& c = sd->committed;
    // screenPos: 与 ComputeContentArea 计算出的 ShmCommitInfo::screenX/Y
    // 同值 (toplevel 虚拟桌面屏幕位置, 即旧 geoX/geoY 的义 1; 非 toplevel
    // 或无 geometry 时恒 0 — 与 fi 同款条件, 无独立算法)
    c.screenX = fi.screenX;
    c.screenY = fi.screenY;
    // parentOffset: 旧 subsurfaceX/Y 的 commit 时点快照 (rel 父偏移义)
    c.parentOffsetX = sd->subsurfaceX;
    c.parentOffsetY = sd->subsurfaceY;
    // frame 属性: 与 SurfaceData / ShmCommitInfo 同值
    c.w = sd->w;
    c.h = sd->h;
    c.shmCommitSerial = fi.shmCommitSerial;
    c.shmFormat = fi.shmFormat;
    c.damageX = sd->damageX;
    c.damageY = sd->damageY;
    c.damageW = sd->damageW;
    c.damageH = sd->damageH;
}

// toplevel 帧更新: 内容裁剪拷贝进 ToplevelState, 建档/首帧事件/格式事件/
// ARGB 掩码/z-order/尺寸上报。协议上 xdg_toplevel 的首个 attach+commit
// 完成 initial show window 语义, 首帧判定靠 hasPosition。
void WaylandServer::UpdateToplevelFrameOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                                ShmCommitInfo& fi, bool& outFirstCommit) {
    outFirstCommit = false;
    if (!sd->hasToplevel) return;
    // Register surface mapping for input focus lookup
    toplevelMgr_.MapToplevelSurface(sd->toplevelId, surfRes);
    auto lk = toplevelMgr_.Lock();
    auto& st = toplevelMgr_.EnsureToplevelLocked(sd->toplevelId);  // 首次 commit 在此建档
    CopyToplevelContent(sd->vpDstW, sd->vpDstH, fi, st.FrameData());
    sd->w = fi.contentW;
    sd->h = fi.contentH;
    st.SetContentSize(fi.contentW, fi.contentH);
    if (sd->toplevelId == desktopRootToplevelId_) {
        desktopCompositor_.IncrementDesktopRootFrameSerial();
    }
    // 自动恢复最小化窗口: 判定/状态改写/日志收口于 ToplevelManager::
    // TryAutoRestoreLocked — "此处已持锁不能调 SetToplevelRestored" 与
    // justRestored 语义 (还原帧 geo 是 Wine 旧原位, 位置跟随须跳过) 的补丁
    // 注释随方法平移 (见 toplevel_manager.cpp); 返回值 = justRestored
    const bool justRestored =
        toplevelMgr_.TryAutoRestoreLocked(sd->toplevelId, fi.contentW, fi.contentH);
    // 首帧判定只认 hasPosition, 不认条目存在
    // (pre-commit 的 SetToplevelMinimized 等路径可能已建档)
    outFirstCommit = !st.HasPosition();
    if (outFirstCommit) {
        st.MarkFirstCommit(fi.screenX, fi.screenY);
        OH_LOG_INFO(LOG_APP, "[MW-MOVE] initial pos tl=%{public}u (%{public}d,%{public}d)",
                    sd->toplevelId, fi.screenX, fi.screenY);
    }
    /*
     * PC 模式: created 延迟到首帧 (此时 wl_shm 格式已确定):
     * - XRGB → "created": 走 WineWindowAbility (multiton 主窗口)
     * - ARGB → "argb_created": 走子窗口 + setWindowMask 异型窗口路线
     *   (2in1 主窗口无 alpha 通道/背景透明被钳制, 实测不可行)
     */
    if (outFirstCommit && Policy().OhosWindowPerToplevel()) {
        char json[160];
        if (fi.shmFormat == 0) {
            snprintf(json, sizeof(json), "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                     fi.screenX, fi.screenY, fi.contentW, fi.contentH);
            OH_LOG_INFO(LOG_APP, "[MW] argb_created tl=%{public}u geo=(%{public}d,%{public}d %{public}dx%{public}d)",
                        sd->toplevelId, fi.screenX, fi.screenY, fi.contentW, fi.contentH);
            FireToplevelEvent(sd->toplevelId, "argb_created", json);
        } else {
            snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", fi.contentW, fi.contentH);
            FireToplevelEvent(sd->toplevelId, "created", json);
        }
    }
    // ARGB 窗口位置同步: Wine 位置为权威 (桌面小部件由 Wine 决定屏幕位置,
    // 普通 PC 窗口后续 commit 忽略 geo, OHOS 窗口管理器为权威 — 完整补丁
    // 说明随方法平移, 见 toplevel_manager.cpp SyncArgbPositionLocked)。
    // 位置应用在 ToplevelManager, argb_move 事件由此处锁内发出 (原时序:
    // 模式/格式/首帧门禁在此判定, 事件锁内发 — 行为逐字)
    if (Policy().OhosWindowPerToplevel() && fi.shmFormat == 0 && !outFirstCommit &&
        toplevelMgr_.SyncArgbPositionLocked(sd->toplevelId, fi.screenX, fi.screenY)) {
        char json[96];
        snprintf(json, sizeof(json), "{\"x\":%d,\"y\":%d}", fi.screenX, fi.screenY);
        FireToplevelEvent(sd->toplevelId, "argb_move", json);
    }
    // 桌面模式后续 commit 的位置同步: 判定 (WineX/Y 快照比较) 与三分支跟随
    // (justRestored 保持 compositor 位置/最小化坐标只记快照/Wine geo 跟随)
    // 收口于 ToplevelManager::SyncDesktopPositionLocked — "compositor 为权威
    // 但 SetWindowPos 必须跟随/move grab 只改 x/y 不改快照/3DMLauncher 边框
    // 残影 (2026-08-11 实测)" 的完整补丁说明随方法平移 (见 toplevel_manager.cpp)
    if (Policy().RootCompositing() && !outFirstCommit) {
        toplevelMgr_.SyncDesktopPositionLocked(sd->toplevelId, fi.screenX, fi.screenY,
                                               justRestored);
    }
    st.MarkDirty();
    // 帧内容序列号: 像素每次 commit 重写时递增 — 桌面局部合成
    // (TakeToplevelFrame damage 裁剪) 以此判定层内容变化
    st.BumpFrameSerial();
    // 记录 shm 格式 (ARGB8888=layered/shaped 有意义 alpha), 变化时通知
    // ArkTS 切换窗口背景 (PC 模式透明背景才能透过 per-pixel alpha)
    // 注意: 首帧必发 argb 事件 (即使首帧就是默认的 XRGB), 与旧的
    // "format 表无此 id" 判定等价 — 只按值比较会吞掉首帧事件
    if (outFirstCommit || st.ShmFormat() != fi.shmFormat) {
        st.SetShmFormat(fi.shmFormat);
        if (Policy().OhosWindowPerToplevel()) {
            char json[32];
            snprintf(json, sizeof(json), "{\"argb\":%d}", fi.shmFormat == 0 ? 1 : 0);
            OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u shm format → %{public}s",
                        sd->toplevelId, fi.shmFormat == 0 ? "ARGB8888" : "XRGB8888");
            FireToplevelEvent(sd->toplevelId, "argb", json);
        }
    }
    // ARGB 窗口掩码: FNV-1a 形状哈希 + 阈值 0/1 剪影生成与 mask 状态更新
    // 收口于 ToplevelManager::UpdateArgbMaskLocked (补丁注释 — 阈值 128
    // 边缘收半像素/形状哈希不变不重建/掩码帧分辨率 — 随方法平移, 见
    // toplevel_manager.cpp)。mask_dirty 事件由此处在锁内发出 (原时序:
    // 事件与状态更新同段同锁, 输出条件逐字不变)
    if (fi.shmFormat == 0 && Policy().OhosWindowPerToplevel()) {
        if (toplevelMgr_.UpdateArgbMaskLocked(sd->toplevelId, st.Pixels(),
                                              fi.contentW, fi.contentH)) {
            FireToplevelEvent(sd->toplevelId, "mask_dirty", "{}");
        }
    }
    // 新 toplevel 加到 Z-order 顶层 (首次入列的全屏优先级取号在
    // AddToZOrder 内部完成, 见 ToplevelState::fsPriority 注释)
    if (Policy().RootCompositing() && sd->toplevelId != desktopRootToplevelId_) {
        toplevelMgr_.EnsureInZOrder(sd->toplevelId);
    }
    OH_LOG_INFO(LOG_APP, "[MW-COMMIT] toplevel #%{public}u frame %{public}dx%{public}d stride=%{public}d stored=%{public}zu",
                sd->toplevelId, fi.contentW, fi.contentH, fi.stride, st.Pixels().size());

    // 检测尺寸变化 -> 通知 ArkTS 调整子窗口: 尺寸变化判定 + 全屏尺寸漂移
    // 补丁 (war3 D3D 模式切换画面缩左上, PLAN §2.5; 补丁注释完整平移, 见
    // toplevel_manager.cpp HandleCommittedSizeLocked) 收口于 ToplevelManager;
    // 锁外动作 (重发 configure) 与判定分离 — NotifyToplevelResize 内部会
    // 再取 toplevelMutex_ (非递归), 不能持锁调用 (见下方自死锁注释)
    const auto sizeEffect = toplevelMgr_.HandleCommittedSizeLocked(
        sd->toplevelId, desktopRootToplevelId_, fi.contentW, fi.contentH,
        outputW_, outputH_);
    if (sizeEffect == ToplevelManager::SizeCommitEffect::ReassertFullscreen) {
        // 必须先解锁: NotifyToplevelResize 内部 IsToplevelFullscreen 会
        // 再取 toplevelMutex_ (非递归 std::mutex), 持锁调用 = 同线程
        // 自死锁 — wayland 事件循环卡死, 输入/帧派发全停 (APP_INPUT_BLOCK,
        // 2026-08-15 war3 全屏黑屏整机卡死的根因)。解锁后不再触碰 st。
        lk.unlock();
        NotifyToplevelResize(sd->toplevelId, outputW_, outputH_);
    } else if (sizeEffect == ToplevelManager::SizeCommitEffect::ResizeEvent) {
        char json[64];
        snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", fi.contentW, fi.contentH);
        OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u size changed: %{public}dx%{public}d max=%{public}s -> ArkTS",
                    sd->toplevelId, fi.contentW, fi.contentH,
                    sd->maximized ? "yes" : "no");
        FireToplevelEvent(sd->toplevelId, "resize", json);
    }
}

// Desktop 模式子窗口 commit → desktop root 识别 (判定逻辑在 DesktopRootManager)
void WaylandServer::CheckDesktopRootOnCommit(SurfaceData* sd, ShmCommitInfo& fi, bool isFirstCommit) {
    if (!Policy().RootCompositing() || !sd->hasToplevel || sd->toplevelId == desktopRootToplevelId_) return;

    // 任务栏身份登记 (app_id 在 xdg_toplevel 创建时已设置, 首次 commit 即有值)
    if (sd->appId == compositor_consts::kAppIdExplorerTaskbar) {
        if (taskbarId_ != sd->toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] taskbar registered: #%{public}u (was #%{public}u)",
                        sd->toplevelId, taskbarId_);
            taskbarId_ = sd->toplevelId;
        }
    }
    DesktopRootManager::CheckRootResult cr;
    {
        auto lk = toplevelMgr_.Lock();
        cr = desktopRootMgr_.CheckRootLocked(sd, isFirstCommit);
        MarkDesktopRootDirtyLocked();
    }
    if (cr.moveRendererTo)
        PluginManager::GetInstance()->MoveRendererToToplevel(cr.moveRendererFrom, cr.moveRendererTo);
    if (cr.fireDesktopRoot) {
        // 新 root 出现: 重置首帧标记, 让 FinishCommit 重新注入 pointer/keyboard
        // focus。热重启 (keep anchor 复用旧 wineserver) 的首帧标记由会话结束
        // 的 ResetSessionState 统一复位; 这里防御会话内 root 切换 — pending
        // root 机制下旧 root 可能尚未销毁, 会话结束清理不会触发, firstFrame_
        // 仍是 true 会导致新 root 首帧不注入 enter, 桌面不激活 (症状: 桌面
        // 只在点击时才刷新, regedit 打开内容不完整, 菜单弹出后释放即关)
        ResetFirstFrame();
        FireToplevelEvent(sd->toplevelId, "desktop_root", "{}");
    }
}

// subsurface 帧分发: Desktop 模式存 layer 在 TakeToplevelFrame 中合成;
// PC 模式登记 popup 伪 toplevel 由 ArkTS 独立子窗口渲染
void WaylandServer::UpdateSubsurfaceOnCommit(SurfaceData* sd, wl_resource* surfRes, ShmCommitInfo& fi) {
    if (!sd->isSubsurface || !sd->parentSurface || sd->pixels.empty()) return;
    auto* parentSd = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
    if (!parentSd || !parentSd->hasToplevel) return;
    if (Policy().SubsurfaceAsLayer()) {
        UpdateSubsurfaceLayerOnCommit(sd, surfRes, parentSd->toplevelId, fi);
    } else {
        UpdatePopupOnCommit(sd, surfRes, parentSd, fi);
    }
}

// ---- 最小化 subsurface offset 补偿: 共享实现已收口到 compositor_utils.h
// (CompensateMinimizedSubsurfaceOffset), desktop_compositor 的 ZC protocolOnly
// 路径共用同一实现 ----

// Desktop 模式: 存 layer, 在 TakeToplevelFrame 中合成 (不进入 per-toplevel 帧缓冲)
void WaylandServer::UpdateSubsurfaceLayerOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                                  uint32_t parentId, ShmCommitInfo& fi) {
    // ARGB 的 opaque 精确判定移到合成快照阶段 (desktop_compositor 融合进
    // 像素拷贝, 单次内存遍历顺带完成) — 曾在此全帧扫描 alpha (800x600
    // 每帧 ~7ms), 堵 wl 事件循环线程, 输入派发被延迟 (WL-T 实测)
    bool opaque = fi.shmFormat != 0;
    auto lk = toplevelMgr_.Lock();
    const auto* pst = toplevelMgr_.FindToplevelLocked(parentId);
    SubsurfaceLayer layer;
    layer.surface = surfRes;  // 用 subsurface 自己的 surface 做 key
    layer.surfaceKey = sd->surfaceKey;
    layer.w = sd->w;
    layer.h = sd->h;
    int32_t sx = sd->subsurfaceX, sy = sd->subsurfaceY;
    CompensateMinimizedSubsurfaceOffset(pst, sx, sy);
    /*
     * Wine 和 compositor 有两套坐标系:
     * - wineX/wineY: Wine 认为的窗口位置 (首次 commit 后不变)
     * - x/y:         compositor 管理的桌面位置 (move grab 后会变)
     *
     * 窗口内菜单: subsurface offset 是相对于窗口内部的 → 跟 compositor 位置
     * 外部菜单 (如任务栏右击): subsurface offset 是 Wine 虚拟屏幕坐标
     *   → 用 Wine 原始位置 (不含 move grab 偏移) 避免双重偏移
     *
     * 判断方法: offset 是否在窗口内容范围内
     */
    int wineX = pst ? pst->WineX() : 0;
    int wineY = pst ? pst->WineY() : 0;
    int compX = pst ? pst->X() : 0;
    int compY = pst ? pst->Y() : 0;
    int compW = pst ? pst->Width() : 0;
    int compH = pst ? pst->Height() : 0;
    bool insideWin = (sx >= 0 && sx < compW && sy >= 0 && sy < compH);
    layer.isExternal = !insideWin;
    layer.localX = sx;
    layer.localY = sy;
    layer.shmCommitSerial = fi.shmCommitSerial;
    if (insideWin) {
        layer.x = compX + sx;
        layer.y = compY + sy;
    } else {
        layer.x = wineX + sx;
        layer.y = wineY + sy;
    }
    layer.parentToplevel = parentId;
    layer.shmFormat = fi.shmFormat;
    layer.opaque = opaque;
    layer.vpDstW = sd->vpDstW; layer.vpDstH = sd->vpDstH;
    layer.dmgX = sd->damageX; layer.dmgY = sd->damageY;
    layer.dmgW = sd->damageW; layer.dmgH = sd->damageH;
    // Upsert layer (pixel buffer rotation handled internally)
    sd->pixels = desktopCompositor_.UpsertSubsurfaceLayer(
        std::move(layer), std::move(sd->pixels));
    MarkDesktopRootDirtyLocked();
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] stored layer %{public}dx%{public}d at (%{public}d,%{public}d) parent=#%{public}u",
                layer.w, layer.h, layer.x, layer.y, parentId);
}

// PC 多窗口模式: popup 登记为伪 toplevel, 由 ArkTS 独立
// OHOS 子窗口渲染。不再 blit 进父 buffer (会被窗口边缘裁剪)。
// 参考 weston/wlroots: subsurface 可越出父 surface 边界,
// compositor 不做父边界裁剪。
void WaylandServer::UpdatePopupOnCommit(SurfaceData* sd, wl_resource* surfRes,
                                        SurfaceData* parentSd, ShmCommitInfo& fi) {
    uint32_t parentId = parentSd->toplevelId;
    /*
     * wp_viewport: Wine popup 的 shm buffer 常按 2 的幂次对齐
     * 填充, 大于真实菜单内容。真实显示尺寸 =
     * min(buffer, set_source, set_destination), 从源矩形原点裁剪
     * (与 desktop 路径 vpDst clamp / toplevel 的 window_geometry
     * 裁剪同语义)。
     *
     * 风险标注 (P2): 父 toplevel 销毁后 popup 会被级联清理
     * (OnToplevelDestroyed), 但若该 popup surface 在父窗口销毁后
     * 恰好又 commit 一帧, 会在此重新登记并向已销毁的 parentId 发
     * popup_show, ArkTS 侧因窗口不存在而积压 (竞态极小, 仅微量
     * 内存)。如需根治可在此处检查 parentId 是否仍存活。
     */
    // 父几何读点 (重构第 5A2 步): 旧读 parentSd->geoX/geoY (即时窗口几何值),
    // 新读 parentSd->committed.contentRect.x/y — 同一写入点 (xs_set_window_geometry
    // 直写快照) 的同步表达式, 逐点等价; 偏移公式语义不变 (相对父内容原点)
    int32_t offX = sd->subsurfaceX - parentSd->committed.contentRect.x;
    int32_t offY = sd->subsurfaceY - parentSd->committed.contentRect.y;
    int dispW = sd->w, dispH = sd->h;
    int cropX = 0, cropY = 0;
    if (sd->vpSrcW > 0 && sd->vpSrcH > 0) {
        cropX = std::max(0, std::min(sd->vpSrcX, sd->w - 1));
        cropY = std::max(0, std::min(sd->vpSrcY, sd->h - 1));
        if (sd->vpSrcW < dispW) dispW = sd->vpSrcW;
        if (sd->vpSrcH < dispH) dispH = sd->vpSrcH;
    }
    if (sd->vpDstW > 0 && sd->vpDstW < dispW) dispW = sd->vpDstW;
    if (sd->vpDstH > 0 && sd->vpDstH < dispH) dispH = sd->vpDstH;
    dispW = std::min(dispW, sd->w - cropX);
    dispH = std::min(dispH, sd->h - cropY);
    // 防御: pixels 须为完整 w*h*4 (subsurface 若设 window_geometry
    // 则 sd->w/h 是 content 尺寸而 pixels 是全 buffer, 不成立)
    const size_t expectSz = static_cast<size_t>(sd->w) * sd->h * 4;
    if (sd->pixels.size() < expectSz) {
        OH_LOG_WARN(LOG_APP, "[MW-POPUP] pixels size mismatch: %{public}zu < %{public}zu (w=%{public}d h=%{public}d), skip frame",
                    sd->pixels.size(), expectSz, sd->w, sd->h);
        return;
    }
    if (dispW <= 0 || dispH <= 0) return;
    uint32_t popupId = 0;
    bool isNew = false;
    bool sizeChanged = false;
    bool posChanged = false;
    /*
     * 全屏主窗口的 GL client surface (war3 D3D 模式切换): wine 把客户区
     * MoveWindow 到模式尺寸 (800x600), client surface 随之缩小, 按 1:1
     * 上报会把画面缩在屏幕左上角。这里把"窗口上报尺寸"与"内容像素尺寸"
     * 解耦: 窗口按全屏输出尺寸上报, FrameData 仍按内容尺寸存 — 渲染侧
     * EglRenderer letterbox 保比例放大上屏, 输入侧 CoordTransform 按同
     * 一 letterbox 逆映射 (与 RA2 主 surface 全屏路径同构)。
     * 判定 = 父全屏 + 偏移 (0,0) + 内容尺寸等于父内容尺寸 (client
     * surface 恰好覆盖整个客户区; 菜单等小 popup 不满足, 不受影响)。
     * 本函数仅 PC 模式到达 (桌面模式走 layer 合成), 不影响 Pad 桌面。
     */
    int winW = dispW, winH = dispH;
    {
        auto lk = toplevelMgr_.Lock();
        auto* pst = toplevelMgr_.FindToplevelLocked(parentId);
        if (pst && pst->IsFullscreen() && offX == 0 && offY == 0 &&
            dispW == pst->Width() && dispH == pst->Height() &&
            outputW_ > 0 && outputH_ > 0 &&
            (dispW < outputW_ || dispH < outputH_)) {
            winW = outputW_;
            winH = outputH_;
        }
        popupId = toplevelMgr_.FindPopupBySurfaceKey(sd->surfaceKey);
        if (popupId == 0) {
            popupId = NextToplevelId();
            isNew = true;
            ToplevelManager::PopupRecord rec;
            rec.popupId = popupId;
            rec.parentToplevel = parentId;
            rec.surface = surfRes;
            rec.surfaceKey = sd->surfaceKey;
            rec.offX = offX;
            rec.offY = offY;
            rec.w = winW;
            rec.h = winH;
            toplevelMgr_.RegisterPopup(popupId, rec);
        } else {
            auto* rec = toplevelMgr_.FindPopup(popupId);
            if (!rec) {
                // 两表不同步 (不应发生): 清孤儿 key, 跳过本帧, 下帧重建
                popupId = 0;
            } else {
                sizeChanged = (rec->w != winW || rec->h != winH);
                posChanged = (rec->offX != offX || rec->offY != offY);
                rec->offX = offX;
                rec->offY = offY;
                rec->w = winW;
                rec->h = winH;
            }
        }
        if (popupId > 0) {
            auto& pbuf = toplevelMgr_.EnsureToplevelLocked(popupId);
            auto& buf = pbuf.FrameData();
            if (cropX == 0 && cropY == 0 && dispW == sd->w && dispH == sd->h) {
                // 无裁剪: 像素双缓冲轮换 (同 desktop layer 做法)
                auto reusablePixels = std::move(buf);
                buf = std::move(sd->pixels);
                sd->pixels = std::move(reusablePixels);
            } else {
                // 裁剪出真实内容区域 (紧凑排列)
                buf.resize(static_cast<size_t>(dispW) * dispH * 4);
                for (int y = 0; y < dispH; y++) {
                    std::memcpy(buf.data() + static_cast<size_t>(y) * dispW * 4,
                                sd->pixels.data() + (static_cast<size_t>(cropY + y) * sd->w + cropX) * 4,
                                static_cast<size_t>(dispW) * 4);
                }
            }
            pbuf.SetContentSize(dispW, dispH);
            pbuf.MarkDirty();
            pbuf.BumpFrameSerial();  // 帧序列号语义: 像素轮换重写即递增
            pbuf.SetShmFormat(fi.shmFormat);
        }
    }
    if (popupId == 0) {
        // 记录异常, 跳过本帧 (下帧按新 popup 重建)
        return;
    }
    if (isNew) {
        toplevelMgr_.MapToplevelSurface(popupId, surfRes);
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"popupId\":%u,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"argb\":%d}",
                 popupId, offX, offY, winW, winH, fi.shmFormat == 0 ? 1 : 0);
        OH_LOG_INFO(LOG_APP, "[MW-POPUP] show popup=#%{public}u parent=#%{public}u off=(%{public}d,%{public}d) %{public}dx%{public}d win=%{public}dx%{public}d (buffer %{public}dx%{public}d src=%{public}d,%{public}d %{public}dx%{public}d dst=%{public}dx%{public}d)",
                    popupId, parentId, offX, offY, dispW, dispH, winW, winH, sd->w, sd->h,
                    sd->vpSrcX, sd->vpSrcY, sd->vpSrcW, sd->vpSrcH, sd->vpDstW, sd->vpDstH);
        FireToplevelEvent(parentId, "popup_show", json);
        return;
    }
    if (sizeChanged) {
        char json[128];
        snprintf(json, sizeof(json), "{\"popupId\":%u,\"w\":%d,\"h\":%d}",
                 popupId, winW, winH);
        FireToplevelEvent(parentId, "popup_resize", json);
    }
    if (posChanged) {
        char json[128];
        snprintf(json, sizeof(json), "{\"popupId\":%u,\"x\":%d,\"y\":%d}",
                 popupId, offX, offY);
        FireToplevelEvent(parentId, "popup_move", json);
    }
}

// commit 收尾: release buffer (协议: 客户端收到 release 才可复写该 buffer),
// 回发 frame callback (协议: commit 生效后 done, 客户端据此帧节流),
// 首帧通知 ArkTS + 预设 pointer/keyboard focus。
void WaylandServer::FinishCommit(SurfaceData* sd, wl_resource* surfRes) {
    // release buffer + frame done
    wl_buffer_send_release(sd->pendingBuffer);

    uint32_t now = static_cast<uint32_t>(time(nullptr) * 1000);
    for (auto* cb : sd->frameCallbacks) {
        wl_callback_send_done(cb, now);
        wl_resource_destroy(cb);
    }
    sd->frameCallbacks.clear();
    sd->pendingBuffer = nullptr;

    // 首帧通知 + 预设 pointer/keyboard focus (参考 HarmonyBox)
    bool expected = false;
    if (firstFrame_.compare_exchange_strong(expected, true)) {
        FireState("active");
        // 预设 focus: Wine 在用户操作前就需要 enter
        // 安全检查: 只有 resource 已创建才注入 (否则 Inject*Enter 内部会 DROP)
        uint32_t tl = sd->toplevelId;
        if (Seat::GetInstance()->HasPointerResource()) {
            InputManager::GetInstance()->InjectPointerEnter(tl, surfRes, wl_fixed_from_int(0), wl_fixed_from_int(0));
        }
        if (Seat::GetInstance()->HasKeyboardResource()) {
            InputManager::GetInstance()->InjectKeyboardEnter(tl, surfRes);
        }
    }
}

void WaylandServer::surface_commit(wl_client*, wl_resource* surfRes) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    auto* self = GetInstance();  // static 回调无 this, 分段均为实例方法
    // WL-T 临时诊断: commit 在 wl 事件循环线程上的占用 — >2ms 打单行,
    // 另按 5s 窗口汇总, 与 LAT-NAPI→LAT-INJ 的 8ms/86ms 对时。
    // 默认关闭 (WINEHUA_FRAME_TRACE=1 开启, 见 perf_utils.h FrameTraceEnabled);
    // 关闭时跳过计时与累加, 零开销
    const bool frameTrace = winehua::FrameTraceEnabled();
    const auto wt0 = frameTrace ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point();
    // NULL buffer → surface 无内容 (unmap)
    if (self->HandleNullBufferCommit(sd, surfRes)) return;

    ShmCommitInfo fi;
    if (self->BeginShmAccess(sd, fi)) {
        self->ComputeContentArea(sd, fi);
        // sd->pixels 的消费方全部是 subsurface 路径 (desktop layer 合成 / PC popup /
        // ARGB 检测); toplevel 的帧走 UpdateToplevelFrameOnCommit 的内容裁剪拷贝,
        // 这里不再为 toplevel 做全量拷贝 (每次 commit 省一整帧 memcpy)
        if (sd->isSubsurface) {
            CopyShmBufferTight(fi, sd->pixels);
        }
        sd->w = fi.contentW;
        sd->h = fi.contentH;
        fi.shmCommitSerial = sd->shmCommitSerial.fetch_add(1, std::memory_order_release) + 1;
        OH_LOG_INFO(LOG_APP, "[MW-COMMIT] surface w=%{public}d h=%{public}d stride=%{public}d stored=%{public}zu content=%{public}dx%{public}d geo=%{public}s",
                    fi.bufW, fi.bufH, fi.stride, sd->pixels.size(), fi.contentW, fi.contentH,
                    sd->committed.hasWindowGeometry ? "yes" : "no");

        // CommittedSurface 快照产出 (重构第 5A2 步): 填入 commit 时点可观察值
        // (screenPos/parentOffset/frame; role 与几何已随协议设置点直写)。
        // 消费端已全部切换到该快照 (geoX/geoY 三义消亡, 见 committed_surface.h)
        self->BuildCommittedSurface(sd, fi);

        bool isFirstCommit = false;
        self->UpdateToplevelFrameOnCommit(sd, surfRes, fi, isFirstCommit);
        self->CheckDesktopRootOnCommit(sd, fi, isFirstCommit);
        self->UpdateSubsurfaceOnCommit(sd, surfRes, fi);
        wl_shm_buffer_end_access(fi.shm);
    }

    self->FinishCommit(sd, surfRes);

    // WL-T 临时诊断 (接函数头): commit 占用统计; frameTrace 关闭时整体跳过
    if (frameTrace) {
        const long long wus = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wt0).count();
        static uint64_t sWinN, sWinUs, sWinMax; static time_t sWinT = time(nullptr);
        sWinN++; sWinUs += wus; if ((uint64_t)wus > sWinMax) sWinMax = wus;
        if (wus > 2000)
            OH_LOG_INFO(LOG_APP, "[WL-T] commit dur=%{public}lldus buf=%{public}dx%{public}d content=%{public}dx%{public}d sub=%{public}d",
                        wus, fi.bufW, fi.bufH, fi.contentW, fi.contentH, sd->isSubsurface ? 1 : 0);
        if (time(nullptr) - sWinT >= 5) {
            OH_LOG_INFO(LOG_APP, "[WL-T] commit 5s: n=%{public}llu avg=%{public}lluus max=%{public}lluus",
                        (unsigned long long)sWinN,
                        sWinN ? (unsigned long long)(sWinUs / sWinN) : 0ull,
                        (unsigned long long)sWinMax);
            sWinN = sWinUs = sWinMax = 0; sWinT = time(nullptr);
        }
    }
}


// -- global 注册 (WaylandServer::Start 调用) --
extern "C" void RegisterWlCoreGlobals(wl_display* display) {
    auto* self = WaylandServer::GetInstance();
    wl_global_create(display, &wl_compositor_interface, 4, self, WaylandServer::compositor_bind);
    wl_global_create(display, &wl_subcompositor_interface, 1, self, WaylandServer::subcompositor_bind);
    wl_global_create(display, &wp_viewporter_interface, 1, self, WaylandServer::viewporter_bind);
    wl_global_create(display, &wl_output_interface, 3, self, WaylandServer::output_bind);
    // dinput 老游戏的指针扩展 (warp 回中/指针约束/relative pointer,
    // 三者均在 PointerExtras::Register 注册, 见 pointer_extras.h 头注释)
    PointerExtras::GetInstance()->Register(display);
    // 解环装配 (重构第 4C1 步): PointerExtras 的 warp 位置同步 (warp 请求 +
    // Lock 约束 cursor_position_hint) 经此回调注入 InputManager::OnPointerWarp —
    // pointer_extras.cpp 不再 include input_manager.h (双向依赖单向化)。
    // 装配在 wl 事件循环启动前 (Start 阶段), 之后回调只在 Wayland 线程读。
    PointerExtras::GetInstance()->SetPointerWarpSink(
        [](wl_resource* surface, double x, double y) {
            InputManager::GetInstance()->OnPointerWarp(surface, x, y);
        });
    // IME 文本输入 (Wine wayland_text_input.c 绑定, 软键盘文字经此注入)
    TextInput::GetInstance()->Register(display);
}
