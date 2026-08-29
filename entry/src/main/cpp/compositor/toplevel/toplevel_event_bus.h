#pragma once
#include <cstdint>
#include <functional>
#include <string>

// ============================================================================
// ToplevelEventBus — toplevel 事件总线 (重构第 5D 步, 行为平价)
//
// 出处: docs/COMPOSITOR_REFACTOR_PLAN.md §三 "ToplevelEventBus 事件 enum 化 +
// JSON 构造单点, NAPI 通道移到订阅者侧" 与 §四阶段5 第 4 条; 对应 §2.3
// "事件名 **22 种 stringly-typed 字符串** ... 散布 5 文件 ~30 处 snprintf
// 手拼 JSON, 无 schema 收口" 与 §2.2 "wayland_server.cpp 的 FireToplevelEvent
// 直接 napi_call_threadsafe_function — NAPI 通道长在 compositor 核心里"。
//
// 本模块收编三件事:
//   1. 事件名 enum 化 — 22 种事件的唯一字符串经 ToplevelEventName 映射,
//      映射表与 ArkTS 消费字符串逐字 (红线: 事件名对外不变; JS 消费见
//      WineWindowManager.ets / PopupWindowManager.ets)。
//   2. JSON 构造单点 — 原散布 5 文件 (wl_core/xdg_shell/wayland_server/
//      plugin_manager) ~30 处的 snprintf 模板全部收到本类的 Json* 静态
//      函数 (模板逐字, 键名/值/顺序不变)。
//   3. NAPI 通道移出 — 投递 Post 与 compositor 核心解耦: 主通道经
//      SetEventSink 注入的字符串式 sink (装配点 = napi_init.cpp
//      SetToplevelCallback, 签名不变); "evt:desktop-ready" 引擎消息旁路
//      (旧 FireToplevelEvent 内直调 gStateTsfn) 移归会话层 PostToplevelEvent
//      (wayland_server.cpp), 经 FireState/stateCb_ 通道投递 — 本模块零
//      napi/wine_process 依赖。
//
// 零依赖约定: 本头可被 host_tests 直连编译 (make test) — EventName /
// Json* 全部头内联, 无 wayland/hilog 依赖。投递语义 (suppress 门禁 /
// [MW] FireToplevel 日志 / sink 派发) 实现在 .cpp (依赖 hilog), 与
// shm_frame_source.{h,cpp} 的"纯函数进头、调度留壳"同模式。
//
// 线程约定: posture 保持旧 FireToplevelEvent 的行为域 — 全部 Post 调用点
// 都在 Wayland 事件循环线程 (wl 线程), bus 内不引入锁/队列; 与旧内联
// 实现的无锁语义等价 (sink_ 在 wl 线程读, 装配一次性注入)。
// ============================================================================

// 22 种 toplevel 事件 (PLAN §2.3 清单; 命名与旧字符串一一对应, 每个事件
// 的 JSON 模板见对应 Json* 函数)。ArkTS 侧按 ToplevelEventName 字符串
// 消费 — 事件名逐字不变 (红线)。
enum class ToplevelEventType : uint32_t {
    // 生命周期
    Created,       // "created"        PC 首帧 ({\"w\":\"h\"}) / 桌面 get_toplevel ({\"w\":640,\"h\":480})
    ArgbCreated,   // "argb_created"   PC 首帧 ARGB 异型窗口
    Destroyed,     // "destroyed"
    // popup (PC 模式菜单)
    PopupHide,     // "popup_hide"
    PopupMove,     // "popup_move"
    PopupShow,     // "popup_show"
    PopupResize,   // "popup_resize"
    // ARGB 异型窗口
    ArgbMove,      // "argb_move"
    Argb,          // "argb"           shm format 切换 (首帧必发)
    MaskDirty,     // "mask_dirty"     0/1 剪影掩码更新
    // 尺寸/格式
    Resize,        // "resize"
    Surface,       // "surface"        renderer surface 物理像素尺寸
    Title,         // "title"
    Limits,        // "limits"         min/max size
    // 窗口状态 (xdg)
    Maximized,     // "maximized"
    Unmaximized,   // "unmaximized"
    Fullscreen,    // "fullscreen"
    Unfullscreen,  // "unfullscreen"
    Minimized,     // "minimized"
    // 交互式移动
    MoveStart,     // "move_start"     xdg_toplevel.move grab 开始
    MoveEnd,       // "move_end"       grab 结束 (或 grab 窗口销毁复位)
    // 会话
    DesktopRoot,   // "desktop_root"   桌面 root 出现 (识别/PromotePending 两路径)
};

// 事件名 → 字符串 (与 ArkTS 消费的原始字符串逐字; 日志与 sink 均用此映射)
inline const char* ToplevelEventName(ToplevelEventType evt) {
    switch (evt) {
        case ToplevelEventType::Created:       return "created";
        case ToplevelEventType::ArgbCreated:   return "argb_created";
        case ToplevelEventType::Destroyed:     return "destroyed";
        case ToplevelEventType::PopupHide:     return "popup_hide";
        case ToplevelEventType::PopupMove:     return "popup_move";
        case ToplevelEventType::PopupShow:     return "popup_show";
        case ToplevelEventType::PopupResize:   return "popup_resize";
        case ToplevelEventType::ArgbMove:      return "argb_move";
        case ToplevelEventType::Argb:          return "argb";
        case ToplevelEventType::MaskDirty:     return "mask_dirty";
        case ToplevelEventType::Resize:        return "resize";
        case ToplevelEventType::Surface:       return "surface";
        case ToplevelEventType::Title:         return "title";
        case ToplevelEventType::Limits:        return "limits";
        case ToplevelEventType::Maximized:     return "maximized";
        case ToplevelEventType::Unmaximized:   return "unmaximized";
        case ToplevelEventType::Fullscreen:    return "fullscreen";
        case ToplevelEventType::Unfullscreen:  return "unfullscreen";
        case ToplevelEventType::Minimized:     return "minimized";
        case ToplevelEventType::MoveStart:     return "move_start";
        case ToplevelEventType::MoveEnd:       return "move_end";
        case ToplevelEventType::DesktopRoot:   return "desktop_root";
    }
    return "unknown";  // 防御: 枚举越界永不发生 (仅编译器告警消噪)
}

// 事件接收方 (订阅侧): 签名保持旧 ToplevelCb (id, eventName, jsonData) —
// napi_init.cpp 的 SetToplevelCallback 装配点零改动。
class ToplevelEventBus {
public:
    using EventSink = std::function<void(uint32_t, const char*, const char*)>;

    // 订阅注入 (wl 线程装配, 一次性; 无锁 — 与旧 toplevelCb_ 同语义)
    void SetEventSink(EventSink sink) { sink_ = std::move(sink); }

    // 首启 wineboot 窗口创建事件抑制 (wine_launch.cpp SetToplevelEventSuppressed
    // 转发): 抑制 created/argb_created, 有 [MW] suppress 日志
    void SetSuppressed(bool on) { suppressed_ = on; }
    bool Suppressed() const { return suppressed_; }

    // 投递单点: 语义 = 旧 WaylandServer::FireToplevelEvent 的
    //   抑制门禁 → [MW] FireToplevel 日志 → sink 派发
    // (desktop_root 的会话侧旁路不在本类 — 见文件头注释 3)。
    // 实现见 toplevel_event_bus.cpp (依赖 hilog)。
    // 调用点线程: wl 线程不变 (红线)。
    void Post(uint32_t id, ToplevelEventType evt, const std::string& json = "{}");

    // ---- JSON 构造单点: 模板与旧各调用点 snprintf 逐字 (键名/值/顺序) ----
    // 无参数事件 (Destroyed/MaskDirty/DesktopRoot/Maximized/Unmaximized/
    // Fullscreen/Unfullscreen/Minimized/MoveStart/MoveEnd) 用默认 "{}" —
    // 与旧调用点传 "{}" 或省略 jsonData 参数等价。

    // created — 两个调用点模板不同 (历史语义, 行为平价):
    //   JsonCreated:      PC 模式首帧 (wl_core.cpp surface_commit, 值 = 内容尺寸)
    //   JsonCreatedDefault: 桌面模式 xdg_get_toplevel (xdg_shell.cpp, 硬编码 640x480,
    //                    ArkTS 据此预留首窗尺寸 — 首帧后走 resize 修正)
    static std::string JsonCreated(int32_t w, int32_t h) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"w\":%d,\"h\":%d}", w, h);
        return buf;
    }
    static std::string JsonCreatedDefault() { return "{\"w\":640,\"h\":480}"; }

    static std::string JsonArgbCreated(int32_t x, int32_t y, int32_t w, int32_t h) {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", x, y, w, h);
        return buf;
    }

    static std::string JsonPopupHide(uint32_t popupId) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"popupId\":%u}", popupId);
        return buf;
    }

    static std::string JsonPopupMove(uint32_t popupId, int32_t x, int32_t y) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"popupId\":%u,\"x\":%d,\"y\":%d}", popupId, x, y);
        return buf;
    }

    static std::string JsonArgbMove(int32_t x, int32_t y) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"x\":%d,\"y\":%d}", x, y);
        return buf;
    }

    // argb01: 1=ARGB8888 (shmFormat==0), 0=XRGB8888 — 调用点传 0/1
    static std::string JsonArgb(int32_t argb01) {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"argb\":%d}", argb01);
        return buf;
    }

    static std::string JsonResize(int32_t w, int32_t h) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"w\":%d,\"h\":%d}", w, h);
        return buf;
    }

    static std::string JsonPopupShow(uint32_t popupId, int32_t x, int32_t y,
                                     int32_t w, int32_t h, int32_t argb01) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"popupId\":%u,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"argb\":%d}",
                 popupId, x, y, w, h, argb01);
        return buf;
    }

    static std::string JsonPopupResize(uint32_t popupId, int32_t w, int32_t h) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"popupId\":%u,\"w\":%d,\"h\":%d}", popupId, w, h);
        return buf;
    }

    // 注意: 旧模板对标题中的引号/反斜杠不做转义 — 既有行为 (Wine 标题含
    // 转义字符时产生非严格 JSON), 行为平价不改, 如需修复须单独提交并过
    // JS 消费侧回归。
    static std::string JsonTitle(const std::string& title) {
        char buf[512];
        snprintf(buf, sizeof(buf), "{\"title\":\"%s\"}", title.c_str());
        return buf;
    }

    static std::string JsonLimits(int32_t minW, int32_t minH, int32_t maxW, int32_t maxH) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"minW\":%d,\"minH\":%d,\"maxW\":%d,\"maxH\":%d}",
                 minW, minH, maxW, maxH);
        return buf;
    }

    static std::string JsonSurface(int32_t w, int32_t h) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"w\":%d,\"h\":%d}", w, h);
        return buf;
    }

private:
    EventSink sink_;
    bool suppressed_ = false;
};
