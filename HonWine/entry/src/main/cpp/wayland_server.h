#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include <unordered_map>

// 最小 Wayland Compositor: wl_compositor + wl_surface + wl_shm
class WaylandServer {
public:
    using StateCb = std::function<void(const char*)>;
    // toplevel 回调: (toplevelId, eventName, jsonData)
    // events: "created", "destroyed", "title", "configure"
    using ToplevelCb = std::function<void(uint32_t, const char*, const char*)>;

    static WaylandServer* GetInstance();

    bool Start(const std::string& socketPath);
    void Stop();

    // EglRenderer 调用: 取最新一帧像素 (deprecated, 用 TakeToplevelFrame)
    bool TakeFrame(std::vector<uint8_t>& outPixels, int& w, int& h);
    // 取指定 toplevel 的最新帧
    bool TakeToplevelFrame(uint32_t toplevelId, std::vector<uint8_t>& outPixels, int& w, int& h);

    // 状态回调 (首帧到达 → 通知 ArkTS)
    void SetStateCallback(StateCb cb) { stateCb_ = std::move(cb); }
    void FireState(const char* s) { if (stateCb_) stateCb_(s); }
    void ResetFirstFrame() { firstFrame_ = false; }

    // toplevel 回调 (xdg_toplevel 生命周期 → 通知 ArkTS 创建/销毁窗口)
    void SetToplevelCallback(ToplevelCb cb) { toplevelCb_ = std::move(cb); }
    void FireToplevelEvent(uint32_t id, const char* event, const char* jsonData = "{}");

    // 生成唯一 toplevel ID
    uint32_t NextToplevelId() { return nextToplevelId_++; }

    // toplevel resource 映射 (用于 SendToplevelClose → xdg_toplevel_send_close)
    void RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl);
    void UnregisterToplevelResource(uint32_t toplevelId);
    void SendToplevelClose(uint32_t toplevelId);

    // ── wayland 协议实现 ──
    static void compositor_bind(wl_client*, void*, uint32_t, uint32_t);
    static void compositor_create_surface(wl_client*, wl_resource*, uint32_t);
    static void compositor_create_region(wl_client*, wl_resource*, uint32_t);

    static void surface_destroy(wl_client*, wl_resource*);
    static void surface_attach(wl_client*, wl_resource*, wl_resource*, int32_t, int32_t);
    static void surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t);
    static void surface_frame(wl_client*, wl_resource*, uint32_t);
    static void surface_commit(wl_client*, wl_resource*);
    static void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
    static void surface_set_input_region(wl_client*, wl_resource*, wl_resource*) {}
    static void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
    static void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
    static void surface_damage_buffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
    static void surface_offset(wl_client*, wl_resource*, int32_t, int32_t) {}

    static void region_destroy(wl_client*, wl_resource*) {}
    static void region_add(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
    static void region_subtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

    /* wl_subcompositor */
    static void subcompositor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void subcompositor_get_subsurface(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*);
    /* wl_subsurface */
    static void subsurface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void subsurface_set_position(wl_client*, wl_resource*, int32_t, int32_t) {}
    static void subsurface_place_above(wl_client*, wl_resource*, wl_resource*) {}
    static void subsurface_place_below(wl_client*, wl_resource*, wl_resource*) {}
    static void subsurface_set_sync(wl_client*, wl_resource*) {}
    static void subsurface_set_desync(wl_client*, wl_resource*) {}

    /* wp_viewporter */
    static void viewporter_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void viewporter_get_viewport(wl_client*, wl_resource*, uint32_t, wl_resource*);
    /* wp_viewport */
    static void viewport_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void viewport_set_source(wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {}
    static void viewport_set_destination(wl_client*, wl_resource*, int32_t, int32_t) {}

    /* Globals bind */
    static void subcompositor_bind(wl_client*, void*, uint32_t, uint32_t);
    static void viewporter_bind(wl_client*, void*, uint32_t, uint32_t);
    static void output_bind(wl_client*, void*, uint32_t, uint32_t);
    /* wl_output */
    static void output_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

private:
    WaylandServer() = default;
    void EventLoop();

    wl_display* display_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // 全局帧缓冲 (deprecated, 保留兼容)
    std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    int width_ = 0, height_ = 0;
    std::atomic<bool> dirty_{false};

    // toplevel 帧缓冲: toplevelId → pixels
    std::mutex toplevelMutex_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> toplevelPixels_;
    std::unordered_map<uint32_t, int> toplevelW_, toplevelH_;
    std::unordered_map<uint32_t, bool> toplevelDirty_;
    std::unordered_map<uint32_t, int> toplevelLastReportedW_, toplevelLastReportedH_;

    StateCb stateCb_;
    ToplevelCb toplevelCb_;
    std::atomic<bool> firstFrame_{false};
    std::atomic<uint32_t> nextToplevelId_{1};
    std::unordered_map<uint32_t, wl_resource*> toplevelResources_;
    std::mutex toplevelResMutex_;
};

// wl_surface 的每个实例携带的数据
struct SurfaceData {
    wl_resource* surface = nullptr;
    wl_resource* pendingBuffer = nullptr;
    std::vector<wl_resource*> frameCallbacks;

    // per-surface pixel buffer
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    bool dirty = false;

    // toplevel identity
    uint32_t toplevelId = 0;
    bool hasToplevel = false;
    std::string title;
    int x = 0, y = 0, winW = 640, winH = 480;

    // xdg_surface window geometry (content area within buffer), 默认全 buffer
    bool hasWindowGeometry = false;
    int geoX = 0, geoY = 0, geoW = 0, geoH = 0;
};
