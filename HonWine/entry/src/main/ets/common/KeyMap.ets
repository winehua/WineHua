// OHOS KeyCode → Linux evdev scancode 映射表
// 参考: Linux include/uapi/linux/input-event-codes.h
//
// Wine 的 keyboard_handle_key() 调用 key2scan(key) 接收 raw evdev code (不需 +8)
// add_xkb_layout() 中使用 key2scan(keyc - 8) 将 xkb keycode 转为 evdev
//
// OHOS keycode 范围 ~2000-2122, evdev 范围 1-255

export const OH2EVDEV: Record<number, number> = {
  // -- 数字行 --
  2000: 11,   // KEYCODE_0 -> KEY_0
  2001: 2,    // KEYCODE_1 -> KEY_1
  2002: 3,    // KEYCODE_2 -> KEY_2
  2003: 4,    // KEYCODE_3 -> KEY_3
  2004: 5,    // KEYCODE_4 -> KEY_4
  2005: 6,    // KEYCODE_5 -> KEY_5
  2006: 7,    // KEYCODE_6 -> KEY_6
  2007: 8,    // KEYCODE_7 -> KEY_7
  2008: 9,    // KEYCODE_8 -> KEY_8
  2009: 10,   // KEYCODE_9 -> KEY_9

  // -- 方向键 --
  2012: 103,  // KEY_DPAD_UP     -> KEY_UP
  2013: 108,  // KEY_DPAD_DOWN   -> KEY_DOWN
  2014: 105,  // KEY_DPAD_LEFT   -> KEY_LEFT
  2015: 106,  // KEY_DPAD_RIGHT  -> KEY_RIGHT

  // -- 字母 A-Z (2017-2042) -> evdev 30-44 + 46-49 --
  2017: 30,   // KEY_A
  2018: 48,   // KEY_B
  2019: 46,   // KEY_C
  2020: 32,   // KEY_D
  2021: 18,   // KEY_E
  2022: 33,   // KEY_F
  2023: 34,   // KEY_G
  2024: 35,   // KEY_H
  2025: 23,   // KEY_I
  2026: 36,   // KEY_J
  2027: 37,   // KEY_K
  2028: 38,   // KEY_L
  2029: 50,   // KEY_M
  2030: 49,   // KEY_N
  2031: 24,   // KEY_O
  2032: 25,   // KEY_P
  2033: 16,   // KEY_Q
  2034: 19,   // KEY_R
  2035: 31,   // KEY_S
  2036: 20,   // KEY_T
  2037: 22,   // KEY_U
  2038: 47,   // KEY_V
  2039: 17,   // KEY_W
  2040: 45,   // KEY_X
  2041: 21,   // KEY_Y
  2042: 44,   // KEY_Z

  // -- 标点/符号 --
  2043: 51,   // KEY_COMMA        -> KEY_COMMA
  2044: 52,   // KEY_PERIOD       -> KEY_DOT
  2056: 41,   // KEY_GRAVE        -> KEY_GRAVE
  2057: 12,   // KEY_MINUS        -> KEY_MINUS
  2058: 13,   // KEY_EQUALS       -> KEY_EQUAL
  2059: 26,   // KEY_LEFT_BRACKET  -> KEY_LEFTBRACE
  2060: 27,   // KEY_RIGHT_BRACKET -> KEY_RIGHTBRACE
  2061: 43,   // KEY_BACKSLASH    -> KEY_BACKSLASH
  2062: 39,   // KEY_SEMICOLON    -> KEY_SEMICOLON
  2063: 40,   // KEY_APOSTROPHE   -> KEY_APOSTROPHE
  2064: 53,   // KEY_SLASH        -> KEY_SLASH

  // -- 修饰键 --
  2045: 56,   // KEY_ALT_LEFT     -> KEY_LEFTALT
  2046: 100,  // KEY_ALT_RIGHT    -> KEY_RIGHTALT
  2047: 42,   // KEY_SHIFT_LEFT   -> KEY_LEFTSHIFT
  2048: 54,   // KEY_SHIFT_RIGHT  -> KEY_RIGHTSHIFT
  2072: 29,   // KEY_CTRL_LEFT    -> KEY_LEFTCTRL
  2073: 97,   // KEY_CTRL_RIGHT   -> KEY_RIGHTCTRL
  2074: 58,   // KEY_CAPS_LOCK    -> KEY_CAPSLOCK
  2076: 125,  // KEY_META_LEFT    -> KEY_LEFTMETA
  2077: 126,  // KEY_META_RIGHT   -> KEY_RIGHTMETA

  // -- 常用键 --
  2049: 15,   // KEY_TAB          -> KEY_TAB
  2050: 57,   // KEY_SPACE        -> KEY_SPACE
  2054: 28,   // KEY_ENTER        -> KEY_ENTER
  2070: 1,    // KEY_ESCAPE       -> KEY_ESC
  2055: 14,   // KEY_DEL          -> KEY_BACKSPACE
  2071: 111,  // KEY_FORWARD_DEL  -> KEY_DELETE
  2067: 139,  // KEY_MENU         -> KEY_MENU

  // -- 导航键 --
  2068: 104,  // KEY_PAGE_UP      -> KEY_PAGEUP
  2069: 109,  // KEY_PAGE_DOWN    -> KEY_PAGEDOWN
  2081: 102,  // KEY_MOVE_HOME    -> KEY_HOME
  2082: 107,  // KEY_MOVE_END     -> KEY_END
  2083: 110,  // KEY_INSERT       -> KEY_INSERT
  2052: 150,  // KEY_EXPLORER     -> KEY_WWW
  2053: 155,  // KEY_ENVELOPE     -> KEY_MAIL
  2084: 159,  // KEY_FORWARD      -> KEY_FORWARD

  // -- F1-F12 --
  2090: 59, 2091: 60,
  2092: 61, 2093: 62,
  2094: 63, 2095: 64,
  2096: 65, 2097: 66,
  2098: 67, 2099: 68,
  2100: 87,   // KEY_F11 -> KEY_F11
  2101: 88,   // KEY_F12 -> KEY_F12

  // -- 小键盘 --
  2102: 69,   // KEY_NUM_LOCK       -> KEY_NUMLOCK
  2103: 82,   // KEY_NUMPAD_0       -> KEY_KP0
  2104: 79,   // KEY_NUMPAD_1       -> KEY_KP1
  2105: 80,   // KEY_NUMPAD_2       -> KEY_KP2
  2106: 81,   // KEY_NUMPAD_3       -> KEY_KP3
  2107: 75,   // KEY_NUMPAD_4       -> KEY_KP4
  2108: 76,   // KEY_NUMPAD_5       -> KEY_KP5
  2109: 77,   // KEY_NUMPAD_6       -> KEY_KP6
  2110: 71,   // KEY_NUMPAD_7       -> KEY_KP7
  2111: 72,   // KEY_NUMPAD_8       -> KEY_KP8
  2112: 73,   // KEY_NUMPAD_9       -> KEY_KP9
  2113: 98,   // KEY_NUMPAD_DIVIDE  -> KEY_KPSLASH
  2114: 55,   // KEY_NUMPAD_MULTIPLY-> KEY_KPASTERISK
  2115: 74,   // KEY_NUMPAD_SUBTRACT-> KEY_KPMINUS
  2116: 78,   // KEY_NUMPAD_ADD     -> KEY_KPPLUS
  2117: 83,   // KEY_NUMPAD_DOT     -> KEY_KPDOT
  2119: 96,   // KEY_NUMPAD_ENTER   -> KEY_KPENTER

  // -- 媒体键 --
  2085: 207,  // KEY_MEDIA_PLAY     -> KEY_PLAY
  2086: 119,  // KEY_MEDIA_PAUSE    -> KEY_PAUSE
  2087: 128,  // KEY_MEDIA_CLOSE    -> KEY_STOP
  2089: 167,  // KEY_MEDIA_RECORD   -> KEY_RECORD

  // -- 系统键 --
  2051: 99,   // KEY_SYM           -> KEY_SYSRQ
}

// 修饰键 OHOS code 列表, 用于追踪 modifier state
export const MODIFIER_CODES: number[] = [
  2045, 2046,   // ALT_LEFT, ALT_RIGHT
  2047, 2048,   // SHIFT_LEFT, SHIFT_RIGHT
  2072, 2073,   // CTRL_LEFT, CTRL_RIGHT
  2076, 2077,   // META_LEFT, META_RIGHT
  2074,          // CAPS_LOCK
]
