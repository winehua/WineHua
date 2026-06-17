#include "wayland_server.h"
#include "seat.h"
#include "input_manager.h"
#include "fps_counter.h"
#include "include/viewporter-server-protocol.h"
#include "include/xdg-shell-server-protocol.h"
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
    while (running_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        wl_event_loop_dispatch(loop, 50); // 50ms timeout
        wl_display_flush_clients(display_);  // dispatch 可能写数据, 之后 flush
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
        delete static_cast<SurfaceData*>(wl_resource_get_user_data(r));
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
    wl_resource* ss = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(ss, &kSubsurfaceImpl, nullptr, nullptr);
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
    // 发送虚拟显示器信息: 1280x720, 96DPI
    wl_output_send_geometry(res, 0, 0, 340, 190,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "Wine", "Virtual",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        1280, 720, 60000);
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
    if (sd && sd->hasToplevel) {
        // 清理 surface 映射
        auto* self = GetInstance();
        {
            std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
            self->toplevelSurfaceMap_.erase(sd->toplevelId);
        }
        // 重置 InputManager 焦点: 防止后续 Inject*Leave 引用已销毁的 surface
        // (否则 Wine 收到 invalid object 协议错误 → 断开连接)
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        self->FireToplevelEvent(sd->toplevelId, "destroyed");
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
        if (sd->hasWindowGeometry && sd->geoW > 0 && sd->geoH > 0) {
            contentW = sd->geoW;
            contentH = sd->geoH;
            contentOffX = sd->geoX;
            contentOffY = sd->geoY;
            OH_LOG_INFO(LOG_APP, "[MW-GEO] using window_geometry: src=%{public}dx%{public}d geo=(%{public}d,%{public}d %{public}dx%{public}d)",
                        w, h, contentOffX, contentOffY, contentW, contentH);
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
            self->toplevelDirty_[sd->toplevelId] = true;
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
    auto it = toplevelDirty_.find(id);
    if (it == toplevelDirty_.end() || !it->second) return false;
    out = toplevelPixels_[id];
    w = toplevelW_[id];
    h = toplevelH_[id];
    toplevelDirty_[id] = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu", id, w, h, out.size());
    return true;
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

// -- toplevelId -> wl_surface 映射 (供 Seat::InjectPointerEnter 查找) --
wl_resource* WaylandServer::GetSurfaceForToplevel(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    auto it = toplevelSurfaceMap_.find(toplevelId);
    if (it != toplevelSurfaceMap_.end()) return it->second;
    return nullptr;
}
