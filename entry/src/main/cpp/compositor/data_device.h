#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ClipboardDevice: wl_data_device_manager 协议实现 (wayland 路线剪贴板)
//
// 背景: wine 的 winewayland.drv 剪贴板完全依赖 wl_data_device_manager
// global (wayland.c:340 缺席即 "clipboard won't work")。x11 路线由
// display_input.c 的 wlroots seat + xwm 桥承接; wayland 路线的合成器
// (compositor/) 此前无 data device, 双向都是死链。本模块补齐:
//   - host -> guest: PostHostClipboard (NAPI 线程入队, loop 线程消费) 建
//     server 端 offer, 向 wine 的 data_device 发 selection 事件; wine
//     延迟渲染, 粘贴时经 offer.receive 取数据
//   - guest -> host: 拦 wl_data_device.set_selection 请求, source.send
//     写入 pipe, loop 线程异步读到 EOF 后经 winehua_host_clipboard_text
//     回调 ArkTS (napi threadsafe fn, 两条路线共享同一入口)
//
// 单客户端模型: 本合成器只有 wine 一个客户端, selection 直发全部
// data_device 资源, 无键盘焦点依赖 (x11 路线的失焦缺口在此不成立)。
// host_text_ 与 post 队列跨会话存活: 冷启动时 wine 晚于 ArkTS 剪贴板
// 同步连接, get_data_device 时按当前文本补发 selection。

class ClipboardDevice {
public:
    static ClipboardDevice* GetInstance();

    // -- 生命周期 (WaylandServer Start/Stop 调用) --
    void Register(wl_display* display);
    void Unregister();

    // host 剪贴板 -> guest (NAPI 线程; Register 前调用则暂存, Register 后随
    // queue 在 loop 线程消费并按规范投递)
    void PostHostClipboard(const char* utf8);

    // 键盘聚焦投递点 (InputInjector::InjectKeyboardEnter 调, loop 线程)。
    // 协议规则 (wayland.xml wl_data_device.selection): selection 事件发给
    // 持键盘焦点的客户端, 焦点切换时向新聚焦客户端补发当前 selection。
    void OnKeyboardFocus(wl_client* client);

    ClipboardDevice() = default;

    // -- 协议请求入口 (loop 线程; public: 接口实现表在 cpp 中部按名引用,
    //    同 seat.cpp 的 public static 模式) --
    static void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void manager_create_data_source(wl_client*, wl_resource*, uint32_t id);
    static void manager_get_data_device(wl_client*, wl_resource*, uint32_t id, wl_resource* seat);
    static void source_offer(wl_client*, wl_resource* res, const char* mime);
    static void source_destroy(wl_client*, wl_resource* res);
    static void source_resource_destroy(wl_resource* res);
    static void source_set_actions(wl_client*, wl_resource* res, uint32_t actions);
    static void device_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*,
                                  wl_resource*, uint32_t);
    static void device_set_selection(wl_client*, wl_resource* res, wl_resource* source, uint32_t serial);
    static void device_release(wl_client*, wl_resource* res);
    static void device_destroy(wl_resource* res);
    static void offer_accept(wl_client*, wl_resource* res, uint32_t serial, const char* mime);
    static void offer_receive(wl_client*, wl_resource* res, const char* mime, int32_t fd);
    static void offer_destroy(wl_client*, wl_resource* res);
    static void offer_finish(wl_client*, wl_resource* res);
    static void offer_set_actions(wl_client*, wl_resource* res, uint32_t actions, uint32_t serial);
    static void offer_resource_destroy(wl_resource* res);

private:
    struct DeviceData {
        ClipboardDevice* self;
        wl_resource* device;
        wl_resource* offer;      // 本 device 当前挂的 host offer
    };
    struct OfferData {
        ClipboardDevice* self;
    };
    struct GuestRead {
        int fd;
        wl_event_source* ev;
        std::vector<char> buf;
        bool discard;
        wl_resource* source;     // 本次读对应的 guest source (所有权裁决用)
    };

    // -- loop 线程内部 --
    void SendHostSelectionTo(DeviceData* dd);      // 向单个 device 发当前 selection
    wl_resource* FindDeviceForClient(wl_client* client);
    static int OnPostPipeReadable(int fd, uint32_t mask, void* data);
    static int OnGuestReadReadable(int fd, uint32_t mask, void* data);
    void GuestReadDestroy(GuestRead* read, bool removeLink);

    wl_display* display_ = nullptr;
    wl_global* global_ = nullptr;
    wl_event_source* pipeSource_ = nullptr;
    int postPipe_[2] = {-1, -1};
    wl_client* focusClient_ = nullptr;            // 最近键盘聚焦的 wine 客户端

    std::vector<DeviceData*> devices_;
    wl_resource* guestSource_ = nullptr;          // wine 最近 set_selection 的 source
    // 各 source 声明的 mime 表 (source_offer 请求填充; loop 线程私有)
    std::map<wl_resource*, std::vector<std::string>> sourceMimes_;
    bool selectionIsHost_ = false;
    std::string hostText_;                        // 宿主剪贴板当前文本 (跨会话存活)
    std::vector<GuestRead*> guestReads_;

    // NAPI 线程 -> loop 线程的 host 文本队列 (mutex + pipe 唤醒, 与 InputQueue 同模式)
    std::mutex postMutex_;
    std::vector<char*> postQueue_;
};
