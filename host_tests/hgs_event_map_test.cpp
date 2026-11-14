// ============================================================================
// hgs_event_map_test — 公共事件契约双向映射对账 (阶段 0 P0.1)
//
// 测试对象: api/hgs_events.h (事件枚举单一来源)。
//   1. ToplevelEventName ↔ ToplevelEventTypeFromName 往返恒等
//   2. 21 个事件名互不重复 (字符串反查/事件总线消费的唯一性前提)
//   3. Unknown 哨兵语义: Name(Unknown)="unknown"; FromName(nullptr/"bogus")
//      = Unknown; Unknown 不在 kAllToplevelEvents 遍历表内
//   4. 红线字符串逐字固化 (ArkTS WineWindowManager/PopupWindowManager/
//      BetterSH WaymonyService 按 event 字符串 switch 消费)
//   5. 显式数值契约: 枚举值 = hgs-exported-api.md §4.2 HgsToplevelEventType
//      (NAPI setToplevelEventCallback 自 1.0 按数值透传, 勿重排)
//
// 构建: make test (host g++ 直连编译, 零 wayland/hilog 依赖 — 纯函数在头)。
// ============================================================================
#include "compositor/toplevel/toplevel_event_bus.h"  // 验证 using 别名链路
#include <cstdio>
#include <cstring>

static int failures = 0;
static int checks = 0;

static void expect_true(const char* what, bool cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

// ---- 1. 往返恒等 + 2. 名字唯一 ----
static void test_roundtrip_and_uniqueness() {
    const int n = static_cast<int>(hgs::kToplevelEventCount);
    expect_true("event count == 21", n == 21);
    for (int i = 0; i < n; i++) {
        hgs::ToplevelEventType evt = hgs::kAllToplevelEvents[i];
        const char* name = hgs::ToplevelEventName(evt);
        char what[128];
        snprintf(what, sizeof(what), "roundtrip [%d] name!=null/unknown", i);
        expect_true(what, name != nullptr && name[0] != '\0' &&
                          strcmp(name, "unknown") != 0);
        snprintf(what, sizeof(what), "roundtrip [%d] FromName(Name)==evt", i);
        expect_true(what, hgs::ToplevelEventTypeFromName(name) == evt);
        for (int j = i + 1; j < n; j++) {
            const char* other = hgs::ToplevelEventName(hgs::kAllToplevelEvents[j]);
            snprintf(what, sizeof(what), "unique '%s' vs '%s'", name, other);
            expect_true(what, strcmp(name, other) != 0);
        }
    }
}

// ---- 3. Unknown 哨兵 ----
static void test_unknown_sentinel() {
    expect_true("Name(Unknown)=='unknown'",
                strcmp(hgs::ToplevelEventName(hgs::ToplevelEventType::Unknown),
                       "unknown") == 0);
    expect_true("FromName(nullptr)==Unknown",
                hgs::ToplevelEventTypeFromName(nullptr) ==
                    hgs::ToplevelEventType::Unknown);
    expect_true("FromName(empty)==Unknown",
                hgs::ToplevelEventTypeFromName("") ==
                    hgs::ToplevelEventType::Unknown);
    expect_true("FromName(bogus)==Unknown",
                hgs::ToplevelEventTypeFromName("not_an_event") ==
                    hgs::ToplevelEventType::Unknown);
    // Unknown 数值 = 0 且不在遍历表内
    checks++;
    for (hgs::ToplevelEventType evt : hgs::kAllToplevelEvents) {
        if (evt == hgs::ToplevelEventType::Unknown) {
            failures++;
            printf("FAIL Unknown must not appear in kAllToplevelEvents\n");
        }
    }
    // 越界数值防御 → "unknown"
    expect_true("Name(out-of-range)=='unknown'",
                strcmp(hgs::ToplevelEventName(
                           static_cast<hgs::ToplevelEventType>(999)),
                       "unknown") == 0);
}

// ---- 4. 红线字符串逐字 (ArkTS 消费) ----
static void test_redline_strings() {
    struct { hgs::ToplevelEventType evt; const char* name; } table[] = {
        { hgs::ToplevelEventType::Created,      "created" },
        { hgs::ToplevelEventType::Destroyed,    "destroyed" },
        { hgs::ToplevelEventType::PopupHide,    "popup_hide" },
        { hgs::ToplevelEventType::PopupMove,    "popup_move" },
        { hgs::ToplevelEventType::PopupShow,    "popup_show" },
        { hgs::ToplevelEventType::PopupResize,  "popup_resize" },
        { hgs::ToplevelEventType::Argb,         "argb" },
        { hgs::ToplevelEventType::Resize,       "resize" },
        { hgs::ToplevelEventType::Surface,      "surface" },
        { hgs::ToplevelEventType::Title,        "title" },
        { hgs::ToplevelEventType::Limits,       "limits" },
        { hgs::ToplevelEventType::Maximized,    "maximized" },
        { hgs::ToplevelEventType::Unmaximized,  "unmaximized" },
        { hgs::ToplevelEventType::Fullscreen,   "fullscreen" },
        { hgs::ToplevelEventType::Unfullscreen, "unfullscreen" },
        { hgs::ToplevelEventType::Minimized,    "minimized" },
        { hgs::ToplevelEventType::Restored,     "restored" },
        { hgs::ToplevelEventType::MoveStart,    "move_start" },
        { hgs::ToplevelEventType::MoveEnd,      "move_end" },
        { hgs::ToplevelEventType::Modal,        "modal" },
        { hgs::ToplevelEventType::DesktopRoot,  "desktop_root" },
    };
    for (const auto& row : table) {
        char what[128];
        snprintf(what, sizeof(what), "redline '%s'", row.name);
        expect_true(what, strcmp(hgs::ToplevelEventName(row.evt), row.name) == 0);
        expect_true(what, strcmp(
            hgs::ToplevelEventName(hgs::ToplevelEventTypeFromName(row.name)),
            row.name) == 0);
    }
}

// ---- 5. 显式数值契约 (勿重排) ----
static void test_explicit_values() {
    expect_true("value Unknown==0",
                static_cast<uint32_t>(hgs::ToplevelEventType::Unknown) == 0);
    expect_true("value Created==1",
                static_cast<uint32_t>(hgs::ToplevelEventType::Created) == 1);
    expect_true("value Destroyed==2",
                static_cast<uint32_t>(hgs::ToplevelEventType::Destroyed) == 2);
    expect_true("value PopupHide==3",
                static_cast<uint32_t>(hgs::ToplevelEventType::PopupHide) == 3);
    expect_true("value PopupMove==4",
                static_cast<uint32_t>(hgs::ToplevelEventType::PopupMove) == 4);
    expect_true("value PopupShow==5",
                static_cast<uint32_t>(hgs::ToplevelEventType::PopupShow) == 5);
    expect_true("value PopupResize==6",
                static_cast<uint32_t>(hgs::ToplevelEventType::PopupResize) == 6);
    expect_true("value Argb==7",
                static_cast<uint32_t>(hgs::ToplevelEventType::Argb) == 7);
    expect_true("value Resize==8",
                static_cast<uint32_t>(hgs::ToplevelEventType::Resize) == 8);
    expect_true("value Surface==9",
                static_cast<uint32_t>(hgs::ToplevelEventType::Surface) == 9);
    expect_true("value Title==10",
                static_cast<uint32_t>(hgs::ToplevelEventType::Title) == 10);
    expect_true("value Limits==11",
                static_cast<uint32_t>(hgs::ToplevelEventType::Limits) == 11);
    expect_true("value Maximized==12",
                static_cast<uint32_t>(hgs::ToplevelEventType::Maximized) == 12);
    expect_true("value Unmaximized==13",
                static_cast<uint32_t>(hgs::ToplevelEventType::Unmaximized) == 13);
    expect_true("value Fullscreen==14",
                static_cast<uint32_t>(hgs::ToplevelEventType::Fullscreen) == 14);
    expect_true("value Unfullscreen==15",
                static_cast<uint32_t>(hgs::ToplevelEventType::Unfullscreen) == 15);
    expect_true("value Minimized==16",
                static_cast<uint32_t>(hgs::ToplevelEventType::Minimized) == 16);
    expect_true("value Restored==17",
                static_cast<uint32_t>(hgs::ToplevelEventType::Restored) == 17);
    expect_true("value MoveStart==18",
                static_cast<uint32_t>(hgs::ToplevelEventType::MoveStart) == 18);
    expect_true("value MoveEnd==19",
                static_cast<uint32_t>(hgs::ToplevelEventType::MoveEnd) == 19);
    expect_true("value Modal==20",
                static_cast<uint32_t>(hgs::ToplevelEventType::Modal) == 20);
    expect_true("value DesktopRoot==21",
                static_cast<uint32_t>(hgs::ToplevelEventType::DesktopRoot) == 21);
}

int main() {
    test_roundtrip_and_uniqueness();
    test_unknown_sentinel();
    test_redline_strings();
    test_explicit_values();
    printf("hgs_event_map_test: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
