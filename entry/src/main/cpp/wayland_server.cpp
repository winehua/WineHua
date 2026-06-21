#include "wayland_server.h"
#include "seat.h"
#include "input_manager.h"
#include "xdg_shell.h"
#include "fps_counter.h"
#include "include/viewporter-server-protocol.h"
#include "include/xdg-shell-server-protocol.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

extern "C" void RegisterXdgShell(wl_display* display);

#undef LOG_TAG
#define LOG_TAG "WL_Server"
#include <hilog/log.h>
#include "plugin_manager.h"

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

// -- 单例 --
WaylandServer* WaylandServer::GetInstance() {
    static WaylandServer s;
    return &s;
}

bool WaylandServer::Start(const std::string& socketPath) {
    if (running_) {
        OH_LOG_WARN(LOG_APP, "[WL] already running");
        return true;
    }

    OH_LOG_INFO(LOG_APP, "[WL] Starting compositor, socket=%{public}s", socketPath.c_str());

    // 清理残留 socket
    unlink(socketPath.c_str());

    // 确保 socket 目录存在
    auto pos = socketPath.find_last_of('/');
    std::string dir = socketPath.substr(0, pos);
    std::string name = socketPath.substr(pos + 1);
    int rc = mkdir(dir.c_str(), 0700);
    OH_LOG_INFO(LOG_APP, "[WL] mkdir(%{public}s) = %{public}d, errno=%{public}d",
                dir.c_str(), rc, errno);

    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);

    display_ = wl_display_create();
    if (!display_) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_create failed, errno=%{public}d", errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] wl_display created");

    if (wl_display_add_socket(display_, name.c_str()) != 0) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_add_socket(%{public}s) failed, errno=%{public}d",
                     name.c_str(), errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] socket added: %{public}s", name.c_str());

    setenv("WAYLAND_DISPLAY", name.c_str(), 1);

    // 注册 global 对象
    wl_global_create(display_, &wl_compositor_interface, 4, this, compositor_bind);
    wl_display_init_shm(display_);
    RegisterXdgShell(display_);
    wl_global_create(display_, &wl_subcompositor_interface, 1, this, subcompositor_bind);
    wl_global_create(display_, &wp_viewporter_interface, 1, this, viewporter_bind);
    wl_global_create(display_, &wl_output_interface, 3, this, output_bind);
    Seat::GetInstance()->Register(display_);
    InputManager::GetInstance()->Initialize(display_);
    OH_LOG_INFO(LOG_APP, "[WL] globals registered (compositor+shm+xdg+subcompositor+viewporter+output+seat+input)");

    running_ = true;
    firstFrame_ = false;
    thread_ = std::thread(&WaylandServer::EventLoop, this);
    OH_LOG_INFO(LOG_APP, "[WL] compositor started OK");
    return true;
}

void WaylandServer::Stop() {
    if (!running_) return;
    running_ = false;
    InputManager::GetInstance()->Shutdown();
    Seat::GetInstance()->Unregister();
    if (display_) wl_display_terminate(display_);
    if (thread_.joinable()) thread_.join();
    if (display_) {
        wl_display_destroy(display_);
        display_ = nullptr;
    }
    firstFrame_ = false;
}

void WaylandServer::EventLoop() {
    int tick = 0;
    while (running_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        int ret = wl_event_loop_dispatch(loop, 50); // 50ms timeout
        if (ret < 0) {
            OH_LOG_ERROR(LOG_APP, "[WL-ERR] event loop error: %{public}s (errno=%{public}d)",
                         strerror(errno), errno);
        }
        wl_display_flush_clients(display_);  // dispatch 可能写数据, 之后 flush

        // 每 30 秒输出一次资源快照 (50ms * 600 = 30s)
        if (++tick % 600 == 0) {
            size_t renderers = PluginManager::GetInstance()->GetRendererCount();
            OH_LOG_INFO(LOG_APP, "[WL-STAT] toplevels=%{public}zu surfaces=%{public}zu renderers=%{public}zu",
                        toplevelResources_.size(), toplevelSurfaceMap_.size(), renderers);
        }
    }
}

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
    wl_resource_set_implementation(surfRes, &kSurfaceImpl, sd, [](wl_resource* r) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(r));
        if (sd && sd->hasToplevel) {
            auto* self = GetInstance();
            {
                std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
                self->toplevelSurfaceMap_.erase(sd->toplevelId);
            }
            InputManager::GetInstance()->OnSurfaceDestroyed(r);
            self->FireToplevelEvent(sd->toplevelId, "destroyed");
        }
        delete sd;
    });
}

void WaylandServer::compositor_create_region(wl_client* client, wl_resource* compRes, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_region_interface,
                                          wl_resource_get_version(compRes), id);
    wl_resource_set_implementation(res, &kRegionImpl, nullptr, nullptr);
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
        OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] subsurface created: child=%p parent=%p",
                    surface, parent);
    }

    // wl_subsurface resource 的 user_data = 子 surface (供 set_position 查找 SurfaceData)
    wl_resource* ss = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(ss, &kSubsurfaceImpl, surface, nullptr);
}

void WaylandServer::subsurface_set_position(wl_client*, wl_resource* ssRes,
                                             int32_t x, int32_t y) {
    // ssRes 的 user_data 是子 surface (在 get_subsurface 中设置)
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(childSurf));
    if (!sd) return;
    sd->subsurfaceX = x;
    sd->subsurfaceY = y;
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] set_position: child=%{public}p pos=(%{public}d,%{public}d)",
                childSurf, x, y);
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
    wl_resource_set_implementation(vp, &kViewportImpl, nullptr, nullptr);
}

// -- output 实现 --
void WaylandServer::output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wl_output bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wl_output_interface, version, id);
    wl_resource_set_implementation(res, &kOutputImpl, data, nullptr);
    auto* self = static_cast<WaylandServer*>(data);
    // 发送虚拟显示器信息 (尺寸由 ArkTS 初始化时设置)
    int32_t pw = self->outputW_, ph = self->outputH_;
    int32_t physW = pw * 340 / 1280;  // 约 96DPI 物理尺寸
    int32_t physH = ph * 190 / 720;
    OH_LOG_INFO(LOG_APP, "[WL] output_bind: %{public}dx%{public}d (phys %{public}dx%{public}d)",
                pw, ph, physW, physH);
    wl_output_send_geometry(res, 0, 0, physW, physH,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "Wine", "Virtual",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        pw, ph, 60000);
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
        {
            std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
            self->toplevelSurfaceMap_.erase(sd->toplevelId);
        }
        // 清理 toplevel 像素数据 + 标记 root dirty
        self->OnToplevelDestroyed(sd->toplevelId);
        // 重置 InputManager 焦点: 防止后续 Inject*Leave 引用已销毁的 surface
        // (否则 Wine 收到 invalid object 协议错误 → 断开连接)
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        // 如果是 desktop root 被销毁，重置 root ID，等待下一个 explorer toplevel
        if (sd->toplevelId == self->GetDesktopRootToplevelId()) {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root toplevel #%{public}u destroyed, clearing root",
                        sd->toplevelId);
            self->SetDesktopRootToplevelId(0);
        }
        self->FireToplevelEvent(sd->toplevelId, "destroyed");
    }
    // subsurface 销毁: 清除 layer + 标记 root dirty 触发重绘 (移除残留像素)
    if (sd && sd->isSubsurface) {
        auto* self = GetInstance();
        std::lock_guard<std::mutex> lk(self->toplevelMutex_);
        for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
            if (it->surface == r) it = self->subsurfaceLayers_.erase(it);
            else ++it;
        }
        if (self->IsDesktopMode() && self->desktopRootToplevelId_ > 0)
            self->toplevelDirty_[self->desktopRootToplevelId_] = true;
    }
    wl_resource_destroy(r);
}

void WaylandServer::surface_attach(wl_client*, wl_resource* surfRes, wl_resource* buffer, int32_t, int32_t) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    sd->pendingBuffer = buffer;
}

void WaylandServer::surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

void WaylandServer::surface_frame(wl_client* client, wl_resource* surfRes, uint32_t cbId) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    wl_resource* cbRes = wl_resource_create(client, &wl_callback_interface, 1, cbId);
    sd->frameCallbacks.push_back(cbRes);
}

void WaylandServer::surface_commit(wl_client*, wl_resource* surfRes) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    if (!sd->pendingBuffer) return;

    // 读取 wl_shm buffer 像素
    wl_shm_buffer* shm = wl_shm_buffer_get(sd->pendingBuffer);
    if (shm) {
        int32_t w = wl_shm_buffer_get_width(shm);
        int32_t h = wl_shm_buffer_get_height(shm);
        int32_t stride = wl_shm_buffer_get_stride(shm);
        int32_t rowBytes = w * 4;  // 紧密排列的每行字节数

        wl_shm_buffer_begin_access(shm);
        const uint8_t* src = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));

        // 确定实际内容区域: 优先用 window_geometry, 否则全 buffer
        int contentW = w, contentH = h;
        int contentOffX = 0, contentOffY = 0;
        int screenX = 0, screenY = 0;  // PAD_MODE: 虚拟桌面位置
        if (sd->hasWindowGeometry && sd->geoW > 0 && sd->geoH > 0) {
            contentW = sd->geoW;
            contentH = sd->geoH;
            if (sd->hasToplevel) {
                // PAD_MODE: toplevel content 永远从 buffer 原点开始,
                // geoX/geoY 是虚拟桌面屏幕位置
                contentOffX = 0;
                contentOffY = 0;
                screenX = sd->geoX;
                screenY = sd->geoY;
            } else {
                // subsurface: geoX/geoY 是相对父 surface 的内容偏移
                contentOffX = sd->geoX;
                contentOffY = sd->geoY;
            }
            OH_LOG_INFO(LOG_APP, "[MW-GEO] using window_geometry: src=%{public}dx%{public}d geo=(%{public}d,%{public}d %{public}dx%{public}d) screen=(%{public}d,%{public}d)",
                        w, h, contentOffX, contentOffY, contentW, contentH, screenX, screenY);
        }

        // 复制像素时 strip stride padding, 只提取 content 区域
        int contentRowBytes = contentW * 4;
        bool strideMismatch = (stride != rowBytes);
        if (strideMismatch) {
            OH_LOG_WARN(LOG_APP, "[MW-STRIDE] stride mismatch! w=%{public}d h=%{public}d stride=%{public}d rowBytes=%{public}d",
                        w, h, stride, rowBytes);
        }
        auto copyTight = [&](std::vector<uint8_t>& dst) {
            dst.resize(contentRowBytes * contentH);
            uint8_t* d = dst.data();
            const uint8_t* rowStart = src + contentOffY * stride + contentOffX * 4;
            for (int32_t y = 0; y < contentH; y++) {
                std::memcpy(d, rowStart + y * stride, contentRowBytes);
                d += contentRowBytes;
            }
        };

        // per-surface buffer (全量, 兼容)
        auto copyTightFull = [&](std::vector<uint8_t>& dst) {
            dst.resize(rowBytes * h);
            uint8_t* d = dst.data();
            for (int32_t y = 0; y < h; y++) {
                std::memcpy(d, src + y * stride, rowBytes);
                d += rowBytes;
            }
        };
        copyTightFull(sd->pixels);
        sd->w = contentW;
        sd->h = contentH;
        sd->dirty = true;
        OH_LOG_INFO(LOG_APP, "[MW-COMMIT] surface w=%{public}d h=%{public}d stride=%{public}d stored=%{public}zu content=%{public}dx%{public}d geo=%{public}s",
                    w, h, stride, sd->pixels.size(), contentW, contentH,
                    sd->hasWindowGeometry ? "yes" : "no");

        auto* self = GetInstance();
        // global framebuffer (deprecated, keep for backward compat)
        {
            std::lock_guard<std::mutex> lk(self->mutex_);
            copyTightFull(self->pixels_);
            self->width_ = w;
            self->height_ = h;
            self->dirty_ = true;
        }
        // toplevel framebuffer
        if (sd->hasToplevel) {
            // Register surface mapping for input focus lookup
            {
                std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
                self->toplevelSurfaceMap_[sd->toplevelId] = surfRes;
            }
            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
            copyTight(self->toplevelPixels_[sd->toplevelId]);
            self->toplevelW_[sd->toplevelId] = contentW;
            self->toplevelH_[sd->toplevelId] = contentH;
            self->toplevelX_[sd->toplevelId] = screenX;
            self->toplevelY_[sd->toplevelId] = screenY;
            self->toplevelDirty_[sd->toplevelId] = true;
            // 新 toplevel 加到 Z-order 顶层
            if (self->IsDesktopMode() && sd->toplevelId != self->desktopRootToplevelId_) {
                auto zit = std::find(self->toplevelZOrder_.begin(), self->toplevelZOrder_.end(), sd->toplevelId);
                if (zit == self->toplevelZOrder_.end()) self->toplevelZOrder_.push_back(sd->toplevelId);
            }
            OH_LOG_INFO(LOG_APP, "[MW-COMMIT] toplevel #%{public}u frame %{public}dx%{public}d stride=%{public}d stored=%{public}zu",
                        sd->toplevelId, contentW, contentH, stride, self->toplevelPixels_[sd->toplevelId].size());

            // 检测尺寸变化 -> 通知 ArkTS 调整子窗口
            int lastW = self->toplevelLastReportedW_[sd->toplevelId];
            int lastH = self->toplevelLastReportedH_[sd->toplevelId];
            if (contentW != lastW || contentH != lastH) {
                self->toplevelLastReportedW_[sd->toplevelId] = contentW;
                self->toplevelLastReportedH_[sd->toplevelId] = contentH;
                char json[64];
                snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", contentW, contentH);
                OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u size changed: %{public}dx%{public}d -> ArkTS",
                            sd->toplevelId, contentW, contentH);
                self->FireToplevelEvent(sd->toplevelId, "resize", json);
            }
        }
        // Desktop 模式: 非 root toplevel commit → 标记 root dirty
        if (self->IsDesktopMode() && sd->hasToplevel &&
            sd->toplevelId != self->GetDesktopRootToplevelId()) {
            uint32_t rootId = self->GetDesktopRootToplevelId();
            // 如果 toplevel 尺寸匹配桌面输出（>=80%），说明 explorer 重建了桌面窗口
            // 更新 root 以使用最新帧
            if (contentW >= self->outputW_ * 8 / 10 && contentH >= self->outputH_ * 8 / 10) {
                OH_LOG_INFO(LOG_APP, "[MW] switching desktop root: #%{public}u -> #%{public}u (%{public}dx%{public}d)",
                            rootId, sd->toplevelId, contentW, contentH);
                // 更新渲染器的 toplevel 映射 (InputManager 坐标转换依赖)
                PluginManager::GetInstance()->MoveRendererToToplevel(rootId, sd->toplevelId);
                self->SetDesktopRootToplevelId(sd->toplevelId);
                // 通知 ArkTS 更新 root ID
                self->FireToplevelEvent(sd->toplevelId, "desktop_root", "{}");
                rootId = sd->toplevelId;
            }
            if (rootId > 0) {
                std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                self->toplevelDirty_[rootId] = true;
            }
        }
        // subsurface: Desktop 模式存 layer 信息, 多窗口模式合成到父 toplevel
        if (sd->isSubsurface && sd->parentSurface && sd->pixels.size() > 0) {
            auto* parentSd = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
            if (parentSd && parentSd->hasToplevel) {
                uint32_t parentId = parentSd->toplevelId;
                if (self->IsDesktopMode()) {
                    // Desktop 模式: 存 layer, 在 TakeToplevelFrame 中合成 (不污染 toplevelPixels_)
                    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                    SubsurfaceLayer layer;
                    layer.surface = surfRes;  // 用 subsurface 自己的 surface 做 key
                    layer.w = sd->w;
                    layer.h = sd->h;
                    layer.x = self->toplevelX_[parentId] + sd->subsurfaceX;
                    layer.y = self->toplevelY_[parentId] + sd->subsurfaceY;
                    layer.pixels = std::move(sd->pixels);
                    // 替换已有 layer
                    bool found = false;
                    for (auto& l : self->subsurfaceLayers_) {
                        if (l.surface == surfRes) { l = std::move(layer); found = true; break; }
                    }
                    if (!found) self->subsurfaceLayers_.push_back(std::move(layer));
                    self->toplevelDirty_[self->desktopRootToplevelId_] = true;
                    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] stored layer %{public}dx%{public}d at (%{public}d,%{public}d) parent=#%{public}u",
                                sd->w, sd->h, layer.x, layer.y, parentId);
                } else {
                    // 多窗口模式: 直接合成到父 toplevel
                    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                    auto it = self->toplevelPixels_.find(parentId);
                    if (it != self->toplevelPixels_.end()) {
                        int parentW = self->toplevelW_[parentId];
                        int parentH = self->toplevelH_[parentId];
                        int popupX = sd->subsurfaceX - parentSd->geoX;
                        int popupY = sd->subsurfaceY - parentSd->geoY;
                        int srcX = (popupX < 0) ? -popupX : 0;
                        int srcY = (popupY < 0) ? -popupY : 0;
                        int dstX = (popupX > 0) ? popupX : 0;
                        int dstY = (popupY > 0) ? popupY : 0;
                        int copyW = sd->w - srcX;
                        int copyH = sd->h - srcY;
                        if (dstX + copyW > parentW) copyW = parentW - dstX;
                        if (dstY + copyH > parentH) copyH = parentH - dstY;
                        if (copyW > 0 && copyH > 0) {
                            auto& targetBuf = it->second;
                            for (int y = 0; y < copyH; y++) {
                                int srcRowStart = ((srcY + y) * sd->w + srcX) * 4;
                                int dstRowStart = ((dstY + y) * parentW + dstX) * 4;
                                std::memcpy(&targetBuf[dstRowStart], &sd->pixels[srcRowStart], copyW * 4);
                            }
                            self->toplevelDirty_[parentId] = true;
                        }
                    }
                }
            }
        }
        wl_shm_buffer_end_access(shm);
    }

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
    if (GetInstance()->firstFrame_.compare_exchange_strong(expected, true)) {
        GetInstance()->FireState("active");
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

// -- 帧数据接口 --
bool WaylandServer::TakeFrame(std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!dirty_) return false;
    out = pixels_;
    w = width_;
    h = height_;
    dirty_ = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] global frame %{public}dx%{public}d px=%{public}zu", w, h, out.size());
    return true;
}

bool WaylandServer::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);

    // Desktop mode: 合成所有非 root toplevel 到 root framebuffer
    // 因为 Wine 不会在子窗口变化时重新提交桌面 surface，
    // 所以必须手动把子窗口像素覆盖到桌面帧上。
    if (desktopMode_ && id == desktopRootToplevelId_) {
        auto rit = toplevelPixels_.find(id);
        if (rit == toplevelPixels_.end()) return false;

        auto dit = toplevelDirty_.find(id);
        if (dit == toplevelDirty_.end() || !dit->second) return false;

        int rootW = toplevelW_[id];
        int rootH = toplevelH_[id];

        // 从干净的 root 帧复制一份用于合成，避免污染 toplevelPixels_[root]
        // （否则下次合成时上次写入的子窗口像素会残留）
        std::vector<uint8_t> composited = rit->second;

        // 按 Z-order 合成: 先合成的在后面, 后合成的覆盖前面
        for (uint32_t childId : toplevelZOrder_) {
            if (childId == id) continue;
            if (toplevelPixels_.find(childId) == toplevelPixels_.end()) continue;
            int cw = toplevelW_[childId], ch = toplevelH_[childId];
            if (cw == rootW && ch == rootH) continue;
            auto& childPx = toplevelPixels_[childId];
            int childW = toplevelW_[childId];
            int childH = toplevelH_[childId];
            int posX = toplevelX_[childId];
            int posY = toplevelY_[childId];
            // 计算子窗口在 root framebuffer 中的可见区域
            int dstX = (posX > 0) ? posX : 0;
            int dstY = (posY > 0) ? posY : 0;
            int srcX = (posX < 0) ? -posX : 0;
            int srcY = (posY < 0) ? -posY : 0;
            int copyW = childW - srcX;
            int copyH = childH - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) continue;
            for (int y = 0; y < copyH; y++) {
                auto* srcRow = &childPx[(srcY + y) * childW * 4];
                auto* dstRow = &composited[(dstY + y) * rootW * 4];
                // toplevel 窗口使用 XRGB8888 (alpha 字节=0)，不加 alpha 过滤
                memcpy(&dstRow[dstX * 4], &srcRow[srcX * 4], copyW * 4);
            }
        }

        // 合成 subsurface 弹出层 (菜单/popup)
        for (auto& layer : subsurfaceLayers_) {
            if (layer.w <= 0 || layer.h <= 0) continue;
            size_t expectSz = (size_t)layer.w * layer.h * 4;
            if (layer.pixels.size() < expectSz) {
                OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                            layer.w, layer.h, layer.pixels.size(), expectSz);
                continue;
            }
            int srcX = (layer.x < 0) ? -layer.x : 0;
            int srcY = (layer.y < 0) ? -layer.y : 0;
            int dstX = (layer.x > 0) ? layer.x : 0;
            int dstY = (layer.y > 0) ? layer.y : 0;
            int copyW = layer.w - srcX;
            int copyH = layer.h - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) continue;
            for (int y = 0; y < copyH; y++) {
                for (int x = 0; x < copyW; x++) {
                    int srcIdx = ((srcY + y) * layer.w + (srcX + x)) * 4;
                    int dstIdx = ((dstY + y) * rootW + (dstX + x)) * 4;
                    // bounds check (防止崩溃导致 mutex 死锁)
                    if (srcIdx + 3 >= (int)layer.pixels.size()) continue;
                    if (dstIdx + 3 >= (int)composited.size()) continue;
                    if (layer.pixels[srcIdx + 3] == 255) continue;
                    memcpy(&composited[dstIdx], &layer.pixels[srcIdx], 4);
                }
            }
        }

        out = std::move(composited);
        w = rootW;
        h = rootH;
        toplevelDirty_[id] = false;
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] desktop root #%{public}u frame %{public}dx%{public}d px=%{public}zu",
                    id, w, h, out.size());
        return true;
    }

    auto it = toplevelDirty_.find(id);
    if (it == toplevelDirty_.end() || !it->second) return false;
    out = toplevelPixels_[id];
    w = toplevelW_[id];
    h = toplevelH_[id];
    toplevelDirty_[id] = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu", id, w, h, out.size());
    return true;
}

void WaylandServer::RaiseToplevel(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    auto it = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), id);
    if (it != toplevelZOrder_.end()) toplevelZOrder_.erase(it);
    toplevelZOrder_.push_back(id);
    toplevelDirty_[desktopRootToplevelId_] = true;
}

void WaylandServer::FireToplevelEvent(uint32_t id, const char* event, const char* jsonData) {
    OH_LOG_INFO(LOG_APP, "[MW] FireToplevel id=%{public}u event=%{public}s data=%{public}s", id, event, jsonData);
    if (toplevelCb_) toplevelCb_(id, event, jsonData);
}

void WaylandServer::RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    toplevelResources_[toplevelId] = tl;
    OH_LOG_INFO(LOG_APP, "[MW] RegisterToplevelResource id=%{public}u tl=%{public}p", toplevelId, tl);
}

void WaylandServer::UnregisterToplevelResource(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    auto it = toplevelResources_.find(toplevelId);
    if (it != toplevelResources_.end()) {
        OH_LOG_INFO(LOG_APP, "[MW] UnregisterToplevelResource id=%{public}u tl=%{public}p (Wine destroyed toplevel)",
                    toplevelId, it->second);
        toplevelResources_.erase(it);
    }
}

void WaylandServer::OnToplevelDestroyed(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    toplevelPixels_.erase(toplevelId);
    toplevelW_.erase(toplevelId);
    toplevelH_.erase(toplevelId);
    toplevelX_.erase(toplevelId);
    toplevelY_.erase(toplevelId);
    toplevelDirty_.erase(toplevelId);
    auto zit = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), toplevelId);
    if (zit != toplevelZOrder_.end()) toplevelZOrder_.erase(zit);
    if (IsDesktopMode() && toplevelId != desktopRootToplevelId_) {
        if (desktopRootToplevelId_ > 0) toplevelDirty_[desktopRootToplevelId_] = true;
    }
}

void WaylandServer::SendToplevelClose(uint32_t toplevelId) {
    wl_resource* tl = nullptr;
    {
        std::lock_guard<std::mutex> lk(toplevelResMutex_);
        auto it = toplevelResources_.find(toplevelId);
        if (it != toplevelResources_.end()) {
            tl = it->second;
            toplevelResources_.erase(it);
        }
    }
    if (tl) {
        OH_LOG_INFO(LOG_APP, "[MW] SendToplevelClose id=%{public}u -> xdg_toplevel_send_close", toplevelId);
        xdg_toplevel_send_close(tl);
    } else {
        OH_LOG_WARN(LOG_APP, "[MW] SendToplevelClose id=%{public}u NOT found", toplevelId);
    }
}

uint32_t WaylandServer::FindToplevelAt(int x, int y) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    uint32_t bestId = desktopRootToplevelId_;
    // 遍历所有子 toplevel, 找包含 (x,y) 且 ID 最大的 (z-order: 越晚创建越在上)
    for (auto& [id, w] : toplevelW_) {
        if (id == desktopRootToplevelId_) continue;
        if (toplevelPixels_.find(id) == toplevelPixels_.end()) continue;
        int tx = toplevelX_[id];
        int ty = toplevelY_[id];
        int tw = toplevelW_[id];
        int th = toplevelH_[id];
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
            if (id > bestId) bestId = id;
        }
    }
    return bestId;
}

void WaylandServer::NotifyWindowRestored(uint32_t toplevelId) {
    // 1. 取 toplevel resource
    wl_resource* tl = nullptr;
    {
        std::lock_guard<std::mutex> lk(toplevelResMutex_);
        auto it = toplevelResources_.find(toplevelId);
        if (it != toplevelResources_.end()) {
            tl = it->second;
        }
    }
    if (!tl) {
        OH_LOG_WARN(LOG_APP, "[MW] NotifyWindowRestored id=%{public}u NOT found", toplevelId);
        return;
    }

    // 2. 遍历 toplevel → xdg_surface → wl_surface → SurfaceData
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    // 3. 清除 minimized 标志
    sd->minimized = false;

    // 4. 发 xdg_toplevel configure (ACTIVE 状态, 告知 Wine 窗口已恢复)
    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(tl, 0, 0, &states);
    wl_array_release(&states);

    // 5. 发 xdg_surface configure
    wl_client* client = wl_resource_get_client(tl);
    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xdg->xdgSurface, wl_display_next_serial(dpy));

    OH_LOG_INFO(LOG_APP, "[MW] NotifyWindowRestored id=%{public}u → xdg configure ACTIVE", toplevelId);
}

void WaylandServer::NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h) {
    wl_resource* tl = nullptr;
    {
        std::lock_guard<std::mutex> lk(toplevelResMutex_);
        auto it = toplevelResources_.find(toplevelId);
        if (it != toplevelResources_.end()) tl = it->second;
    }
    if (!tl) return;

    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg) return;

    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));

    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (sd && sd->maximized) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    xdg_toplevel_send_configure(tl, w, h, &states);
    wl_array_release(&states);

    wl_client* client = wl_resource_get_client(tl);
    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xdg->xdgSurface, wl_display_next_serial(dpy));

    OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize id=%{public}u → %{public}dx%{public}d maximized=%{public}s",
                toplevelId, w, h, (sd && sd->maximized) ? "yes" : "no");
}

// -- toplevelId -> wl_surface 映射 (供 Seat::InjectPointerEnter 查找) --
wl_resource* WaylandServer::GetSurfaceForToplevel(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    auto it = toplevelSurfaceMap_.find(toplevelId);
    if (it != toplevelSurfaceMap_.end()) return it->second;
    OH_LOG_WARN(LOG_APP, "[MW] GetSurfaceForToplevel #%{public}u NOT FOUND (map size=%{public}zu)",
                toplevelId, toplevelSurfaceMap_.size());
    return nullptr;
}
