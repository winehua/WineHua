// ============================================================================
// native_keymap_test — OHOS KeyCode → evdev 对账 (阶段 1 N1)
//
// 测试对象: input/native_keymap.{h,cpp} (移植自 KeyMap.ets OH2EVDEV)。
// 黄金值 = KeyMap.ets 的条目逐字; 任何漂移即红 (原生输入后端与 ArkTS
// 后端必须产出相同 evdev code, 否则同一按键在两种 backend 下行为不同)。
//
// 构建: make test (host g++ 直连编译, 零平台依赖 — 纯函数)。
// ============================================================================
#include "input/native_keymap.h"
#include <cstdio>

static int failures = 0;
static int checks = 0;

static void expect_eq(const char* what, int32_t got, int32_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got=%d want=%d\n", what, got, want);
    }
}

int main() {
    // 数字行 0-9 (OHOS 2000-2009 → evdev 11,2..10)
    const int32_t digits[] = {11, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int i = 0; i < 10; i++) {
        expect_eq("digit", hgs::input::OhosKeyCodeToEvdev(2000 + i), digits[i]);
    }

    // 字母 A-Z (2017-2042 → 30,48,46,32,18,33,34,35,23,36,37,38,50,49,
    //                      24,25,16,19,31,20,22,47,17,45,21,44)
    const int32_t letters[] = {
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    };
    for (int i = 0; i < 26; i++) {
        expect_eq("letter", hgs::input::OhosKeyCodeToEvdev(2017 + i), letters[i]);
    }

    // 方向键
    expect_eq("UP", hgs::input::OhosKeyCodeToEvdev(2012), 103);
    expect_eq("DOWN", hgs::input::OhosKeyCodeToEvdev(2013), 108);
    expect_eq("LEFT", hgs::input::OhosKeyCodeToEvdev(2014), 105);
    expect_eq("RIGHT", hgs::input::OhosKeyCodeToEvdev(2015), 106);

    // 修饰键
    expect_eq("LEFTALT", hgs::input::OhosKeyCodeToEvdev(2045), 56);
    expect_eq("RIGHTALT", hgs::input::OhosKeyCodeToEvdev(2046), 100);
    expect_eq("LEFTSHIFT", hgs::input::OhosKeyCodeToEvdev(2047), 42);
    expect_eq("RIGHTSHIFT", hgs::input::OhosKeyCodeToEvdev(2048), 54);
    expect_eq("LEFTCTRL", hgs::input::OhosKeyCodeToEvdev(2072), 29);
    expect_eq("RIGHTCTRL", hgs::input::OhosKeyCodeToEvdev(2073), 97);
    expect_eq("CAPSLOCK", hgs::input::OhosKeyCodeToEvdev(2074), 58);
    expect_eq("LEFTMETA", hgs::input::OhosKeyCodeToEvdev(2076), 125);
    expect_eq("RIGHTMETA", hgs::input::OhosKeyCodeToEvdev(2077), 126);

    // 常用键
    expect_eq("TAB", hgs::input::OhosKeyCodeToEvdev(2049), 15);
    expect_eq("SPACE", hgs::input::OhosKeyCodeToEvdev(2050), 57);
    expect_eq("ENTER", hgs::input::OhosKeyCodeToEvdev(2054), 28);
    expect_eq("ESC", hgs::input::OhosKeyCodeToEvdev(2070), 1);
    expect_eq("BACKSPACE", hgs::input::OhosKeyCodeToEvdev(2055), 14);
    expect_eq("DELETE", hgs::input::OhosKeyCodeToEvdev(2071), 111);
    expect_eq("MENU", hgs::input::OhosKeyCodeToEvdev(2067), 139);

    // 导航
    expect_eq("PAGEUP", hgs::input::OhosKeyCodeToEvdev(2068), 104);
    expect_eq("PAGEDOWN", hgs::input::OhosKeyCodeToEvdev(2069), 109);
    expect_eq("HOME", hgs::input::OhosKeyCodeToEvdev(2081), 102);
    expect_eq("END", hgs::input::OhosKeyCodeToEvdev(2082), 107);
    expect_eq("INSERT", hgs::input::OhosKeyCodeToEvdev(2083), 110);

    // 标点
    expect_eq("COMMA", hgs::input::OhosKeyCodeToEvdev(2043), 51);
    expect_eq("PERIOD", hgs::input::OhosKeyCodeToEvdev(2044), 52);
    expect_eq("GRAVE", hgs::input::OhosKeyCodeToEvdev(2056), 41);
    expect_eq("MINUS", hgs::input::OhosKeyCodeToEvdev(2057), 12);
    expect_eq("EQUAL", hgs::input::OhosKeyCodeToEvdev(2058), 13);
    expect_eq("LBRACKET", hgs::input::OhosKeyCodeToEvdev(2059), 26);
    expect_eq("RBRACKET", hgs::input::OhosKeyCodeToEvdev(2060), 27);
    expect_eq("BACKSLASH", hgs::input::OhosKeyCodeToEvdev(2061), 43);
    expect_eq("SEMICOLON", hgs::input::OhosKeyCodeToEvdev(2062), 39);
    expect_eq("APOSTROPHE", hgs::input::OhosKeyCodeToEvdev(2063), 40);
    expect_eq("SLASH", hgs::input::OhosKeyCodeToEvdev(2064), 53);

    // F1-F12
    const int32_t fkeys[] = {59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88};
    for (int i = 0; i < 12; i++) {
        expect_eq("Fkey", hgs::input::OhosKeyCodeToEvdev(2090 + i), fkeys[i]);
    }

    // 小键盘
    expect_eq("NUMLOCK", hgs::input::OhosKeyCodeToEvdev(2102), 69);
    expect_eq("KP0", hgs::input::OhosKeyCodeToEvdev(2103), 82);
    expect_eq("KP9", hgs::input::OhosKeyCodeToEvdev(2112), 73);
    expect_eq("KPSLASH", hgs::input::OhosKeyCodeToEvdev(2113), 98);
    expect_eq("KPASTERISK", hgs::input::OhosKeyCodeToEvdev(2114), 55);
    expect_eq("KPMINUS", hgs::input::OhosKeyCodeToEvdev(2115), 74);
    expect_eq("KPPLUS", hgs::input::OhosKeyCodeToEvdev(2116), 78);
    expect_eq("KPDOT", hgs::input::OhosKeyCodeToEvdev(2117), 83);
    expect_eq("KPENTER", hgs::input::OhosKeyCodeToEvdev(2119), 96);

    // 未知键: 0 (调用方丢弃)
    expect_eq("unknown-low", hgs::input::OhosKeyCodeToEvdev(1), 0);
    expect_eq("unknown-high", hgs::input::OhosKeyCodeToEvdev(9999), 0);
    expect_eq("unknown-zero", hgs::input::OhosKeyCodeToEvdev(0), 0);

    // 修饰键判定
    checks++;
    if (!hgs::input::IsModifierKeyCode(2045) || !hgs::input::IsModifierKeyCode(2074)) {
        failures++;
        printf("FAIL modifier set missing alt/caps\n");
    }
    checks++;
    if (hgs::input::IsModifierKeyCode(2017)) {
        failures++;
        printf("FAIL 'A' classified as modifier\n");
    }

    printf("native_keymap_test: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
