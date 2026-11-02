// ============================================================================
// toplevel_event_test — ToplevelEventBus 纯函数对账 (行为了契约化)
//
// 测试对象: compositor/toplevel/toplevel_event_bus.h 的事件名映射与 JSON 构造单点。
// 黄金值 = 当前头内模板逐字 (2026-10-31 与现行实现对账: display-route 重构
// 移除 ArgbCreated/ArgbMove — created 带坐标改走 JsonCreatedAt (键序 w,h,x,y),
// 新增 Restored/Modal 与 JsonModal)。事件名/键名/值/顺序是 ArkTS 消费红线
// (WineWindowManager.ets / PopupWindowManager.ets 按事件名与字段消费),
// 任何模板漂移即红。
//
// 构建: make test (host g++ 直连编译, 零 wayland/hilog 依赖 — 纯函数在头)。
// ============================================================================
#include "compositor/toplevel/toplevel_event_bus.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static int checks = 0;

static void eq_str(const char* what, const std::string& got, const std::string& want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s\n  got : %s\n  want: %s\n", what, got.c_str(), want.c_str());
    }
}

// ---- ToplevelEventName: 21 种事件名与 ArkTS 消费字符串逐字 ----
// (display-route 重构移除 ArgbCreated/ArgbMove — created 带坐标走
//  JsonCreatedAt, 移动并入 position 更新; 新增 Restored/Modal)
static void test_event_names() {
    eq_str("name(Created)", ToplevelEventName(ToplevelEventType::Created), "created");
    eq_str("name(Destroyed)", ToplevelEventName(ToplevelEventType::Destroyed), "destroyed");
    eq_str("name(PopupHide)", ToplevelEventName(ToplevelEventType::PopupHide), "popup_hide");
    eq_str("name(PopupMove)", ToplevelEventName(ToplevelEventType::PopupMove), "popup_move");
    eq_str("name(PopupShow)", ToplevelEventName(ToplevelEventType::PopupShow), "popup_show");
    eq_str("name(PopupResize)", ToplevelEventName(ToplevelEventType::PopupResize), "popup_resize");
    eq_str("name(Argb)", ToplevelEventName(ToplevelEventType::Argb), "argb");
    eq_str("name(Resize)", ToplevelEventName(ToplevelEventType::Resize), "resize");
    eq_str("name(Surface)", ToplevelEventName(ToplevelEventType::Surface), "surface");
    eq_str("name(Title)", ToplevelEventName(ToplevelEventType::Title), "title");
    eq_str("name(Limits)", ToplevelEventName(ToplevelEventType::Limits), "limits");
    eq_str("name(Maximized)", ToplevelEventName(ToplevelEventType::Maximized), "maximized");
    eq_str("name(Unmaximized)", ToplevelEventName(ToplevelEventType::Unmaximized), "unmaximized");
    eq_str("name(Fullscreen)", ToplevelEventName(ToplevelEventType::Fullscreen), "fullscreen");
    eq_str("name(Unfullscreen)", ToplevelEventName(ToplevelEventType::Unfullscreen), "unfullscreen");
    eq_str("name(Minimized)", ToplevelEventName(ToplevelEventType::Minimized), "minimized");
    eq_str("name(Restored)", ToplevelEventName(ToplevelEventType::Restored), "restored");
    eq_str("name(MoveStart)", ToplevelEventName(ToplevelEventType::MoveStart), "move_start");
    eq_str("name(MoveEnd)", ToplevelEventName(ToplevelEventType::MoveEnd), "move_end");
    eq_str("name(Modal)", ToplevelEventName(ToplevelEventType::Modal), "modal");
    eq_str("name(DesktopRoot)", ToplevelEventName(ToplevelEventType::DesktopRoot), "desktop_root");
}

// ---- JSON 模板: 与各调用点旧 snprintf 逐字 (键名/值/顺序) ----
static void test_json_templates() {
    // created: PC 首帧 (wl_core.cpp surface_commit)
    eq_str("JsonCreated", ToplevelEventBus::JsonCreated(800, 600), "{\"w\":800,\"h\":600}");
    eq_str("JsonCreated edge-neg", ToplevelEventBus::JsonCreated(-1, 0), "{\"w\":-1,\"h\":0}");
    // created: 桌面 xdg_get_toplevel (xdg_shell.cpp, 硬编码 640x480)
    eq_str("JsonCreatedDefault", ToplevelEventBus::JsonCreatedDefault(), "{\"w\":640,\"h\":480}");
    // created 带坐标 (D52: x11 多窗按 guest 位置摆放; 键序 w,h,x,y —
    // x/y=0 表示不定位, 与 wayland JsonCreated 消费语义相容)
    eq_str("JsonCreatedAt", ToplevelEventBus::JsonCreatedAt(800, 600, 10, 20),
           "{\"w\":800,\"h\":600,\"x\":10,\"y\":20}");
    eq_str("JsonCreatedAt at-origin", ToplevelEventBus::JsonCreatedAt(0, 1, 0, 0),
           "{\"w\":0,\"h\":1,\"x\":0,\"y\":0}");
    eq_str("JsonCreatedAt neg", ToplevelEventBus::JsonCreatedAt(0, 1, -5, -6),
           "{\"w\":0,\"h\":1,\"x\":-5,\"y\":-6}");
    // popup_hide (5 处调用点同模板)
    eq_str("JsonPopupHide", ToplevelEventBus::JsonPopupHide(7), "{\"popupId\":7}");
    eq_str("JsonPopupHide max", ToplevelEventBus::JsonPopupHide(4294967295u),
           "{\"popupId\":4294967295}");
    // popup_move
    eq_str("JsonPopupMove", ToplevelEventBus::JsonPopupMove(7, 100, -200),
           "{\"popupId\":7,\"x\":100,\"y\":-200}");
    // argb (0/1)
    eq_str("JsonArgb 1", ToplevelEventBus::JsonArgb(1), "{\"argb\":1}");
    eq_str("JsonArgb 0", ToplevelEventBus::JsonArgb(0), "{\"argb\":0}");
    // resize
    eq_str("JsonResize", ToplevelEventBus::JsonResize(1400, 920), "{\"w\":1400,\"h\":920}");
    // popup_show (6 字段, argb 末位)
    eq_str("JsonPopupShow", ToplevelEventBus::JsonPopupShow(3, 12, 34, 640, 480, 1),
           "{\"popupId\":3,\"x\":12,\"y\":34,\"w\":640,\"h\":480,\"argb\":1}");
    // popup_resize
    eq_str("JsonPopupResize", ToplevelEventBus::JsonPopupResize(3, 640, 480),
           "{\"popupId\":3,\"w\":640,\"h\":480}");
    // title (旧模板对引号/反斜杠不转义 — 既有行为逐字)
    eq_str("JsonTitle", ToplevelEventBus::JsonTitle("未命名 - 记事本"),
           "{\"title\":\"未命名 - 记事本\"}");
    eq_str("JsonTitle empty", ToplevelEventBus::JsonTitle(""), "{\"title\":\"\"}");
    // limits
    eq_str("JsonLimits", ToplevelEventBus::JsonLimits(320, 200, 1400, 920),
           "{\"minW\":320,\"minH\":200,\"maxW\":1400,\"maxH\":920}");
    // surface
    eq_str("JsonSurface", ToplevelEventBus::JsonSurface(1280, 800), "{\"w\":1280,\"h\":800}");
    // modal (键序 modal,owner,tl,dx,dy,w,h)
    eq_str("JsonModal set", ToplevelEventBus::JsonModal(7, 3, 1, 10, 20, 640, 480),
           "{\"modal\":1,\"owner\":3,\"tl\":7,\"dx\":10,\"dy\":20,\"w\":640,\"h\":480}");
    eq_str("JsonModal clear", ToplevelEventBus::JsonModal(7, 0, 0, 0, 0, 0, 0),
           "{\"modal\":0,\"owner\":0,\"tl\":7,\"dx\":0,\"dy\":0,\"w\":0,\"h\":0}");
}

// 无 payload 事件的 "{}" 语义: 旧调用点传字面量 "{}" 或省略 jsonData 参数
// (均产生 "{}"), 新形态经 Post 默认参数 const std::string& json = "{}" 承载 —
// 该签名为接口级等价 (Post 本体 in toplevel_event_bus.cpp, 依赖 hilog,
// 不属 host 可编译面; 默认参数值在此声明为契约)。

static void test_full_coverage() {
    // 21 种枚举全在 EventName 映射内 (编译器已保证 switch 完整性; 此处
    // 再逐一遍历确认 21 个名字均为非空且长度>0)
    int n = 0;
    for (uint32_t i = 0; i < 21; i++) {
        const char* name = ToplevelEventName(static_cast<ToplevelEventType>(i));
        checks++;
        if (name == nullptr || name[0] == '\0' || strcmp(name, "unknown") == 0) {
            failures++;
            printf("FAIL coverage enum=%u -> '%s'\n", i, name);
        }
        n++;
    }
    printf("event-name coverage: %d\n", n);
}

int main() {
    test_event_names();
    test_json_templates();
    test_full_coverage();
    printf("toplevel_event_test: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
