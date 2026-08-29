#include "compositor/toplevel_event_bus.h"

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"
#include <hilog/log.h>

// ============================================================================
// ToplevelEventBus 投递实现 (重构第 5D 步, 行为平价)
//
// 与旧 WaylandServer::FireToplevelEvent (wayland_server.cpp, 本步前状态)
// 逐字对应:
//   ① 抑制门禁 — suppressed_ (原 toplevelEventSuppressed_) && created/
//      argb_created → [MW] suppress 日志 (文本/条件/短路顺序逐字);
//   ② [MW] FireToplevel id=... event=... data=... 日志 — eventName 经
//      ToplevelEventName 映射为旧字符串, id/event/data 占位符逐字;
//   ③ sink 派发 — EventSink 即旧 toplevelCb_ (napi_init SetToplevelCallback
//      注入的 lambda, 签名 (id, eventName, jsonData) 未变);
//   ④ desktop_root 会话侧旁路 (MarkDesktopShellProcesses + evt:desktop-ready
//      补票) 不在本类 — 移归 WaylandServer::PostToplevelEvent (会话层编排,
//      wayland_server.cpp), 使本模块零 napi/wine_process 依赖 (PLAN §2.2)。
//
// 行为平价: 事件名逐字 (ToplevelEventName 映射表)、日志文本/顺序逐字、
// 抑制门禁条件逐字、sink 回调先后时序不变; 唯一结构变化 = NAPI 移出
// (desktop_root 分支不再直调 gStateTsfn, 见 PostToplevelEvent 注释)。
// 调用点线程域不变 (wl 线程), bus 内无锁无队列 (与旧内联实现同语义)。
// ============================================================================

void ToplevelEventBus::Post(uint32_t id, ToplevelEventType evt,
                            const std::string& json) {
    const char* name = ToplevelEventName(evt);
    // ① 首启 wineboot 抑制窗口创建事件 (PC 窗口模式): 抑制 created/
    //    argb_created 后 ArkTS 不启动 WineWindowAbility, wineboot 等待窗
    //    不出现在系统桌面。功能不受影响 — wine.inf 安装不依赖窗口显示;
    //    wineboot 退出的 destroyed 事件照常派发, ArkTS 对未知 toplevel id
    //    的销毁容忍。原条件/短路顺序/日志逐字。
    if (suppressed_ &&
        (evt == ToplevelEventType::Created || evt == ToplevelEventType::ArgbCreated)) {
        OH_LOG_INFO(LOG_APP, "[MW] suppress %{public}s tl=%{public}u (wineboot init)",
                    name, id);
        return;
    }
    // ② 派发日志 (原 FireToplevelEvent 的直接日志, 文本/占位符逐字)
    OH_LOG_INFO(LOG_APP, "[MW] FireToplevel id=%{public}u event=%{public}s data=%{public}s",
                id, name, json.c_str());
    // ③ 主事件通道 (ArkTS toplevel 回调; 装配点 napi_init SetToplevelCallback)
    if (sink_) sink_(id, name, json.c_str());
}
