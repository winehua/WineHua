#include "seat.h"
#include "keymap_xkb.h"
#include "input_manager.h"
#include "wayland_server.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#undef LOG_TAG
#define LOG_TAG "WL_Seat"
#include <hilog/log.h>

// -- wl_seat 接口实现表 --
static const struct wl_seat_interface kSeatImpl = {
    .get_pointer  = Seat::seat_get_pointer,
    .get_keyboard = Seat::seat_get_keyboard,
    .get_touch    = Seat::seat_get_touch,
    .release      = Seat::seat_release,
};

// -- wl_pointer 接口实现表 --
static void ptr_set_cursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {}
static void ptr_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static const struct wl_pointer_interface kPointerImpl = {
    .set_cursor = ptr_set_cursor,
    .release    = ptr_release,
};

// -- wl_keyboard 接口实现表 --
static void kbd_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static const struct wl_keyboard_interface kKeyboardImpl = {
    .release = kbd_release,
};

// -- wl_touch 接口实现表 --
static void tch_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static const struct wl_touch_interface kTouchImpl = {
    .release = tch_release,
};

// -- 单例 --
Seat* Seat::GetInstance() {
    static Seat s;
    return &s;
}

// -- 资源访问 --
wl_resource* Seat::GetPointerResource() {
    std::lock_guard<std::mutex> lk(ptrResMutex_);
    return pointerResources_.empty() ? nullptr : pointerResources_.back();
}
wl_resource* Seat::GetKeyboardResource() {
    std::lock_guard<std::mutex> lk(kbdResMutex_);
    return keyboardResources_.empty() ? nullptr : keyboardResources_.back();
}

std::vector<wl_resource*> Seat::GetAllPointerResources() {
    std::lock_guard<std::mutex> lk(ptrResMutex_);
    return pointerResources_;  // copy
}

std::vector<wl_resource*> Seat::GetAllKeyboardResources() {
    std::lock_guard<std::mutex> lk(kbdResMutex_);
    return keyboardResources_;  // copy
}

// ========================================================================
//  生命周期 (wyland_server Start/Stop 调用)
// ========================================================================

void Seat::Register(wl_display* display) {
    if (global_) {
        OH_LOG_WARN(LOG_APP, "[Seat] already registered");
        return;
    }
    display_ = display;
    global_ = wl_global_create(display, &wl_seat_interface, 5, this, seat_bind);
    OH_LOG_INFO(LOG_APP, "[Seat] wl_seat global registered OK");
}

void Seat::Unregister() {
    if (global_) {
        wl_global_destroy(global_);
        global_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(ptrResMutex_);
        pointerResources_.clear();
        ptrCount_.store(0);
    }
    {
        std::lock_guard<std::mutex> lk(kbdResMutex_);
        keyboardResources_.clear();
        kbdCount_.store(0);
    }
    seatResource_ = nullptr;
    display_ = nullptr;
    OH_LOG_INFO(LOG_APP, "[Seat] unregistered OK");
}

// ========================================================================
//  wl_seat 协议方法
// ========================================================================

void Seat::seat_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<Seat*>(data);
    uint32_t v = std::min(version, 5u);

    wl_resource* res = wl_resource_create(client, &wl_seat_interface, v, id);
    wl_resource_set_implementation(res, &kSeatImpl, self, nullptr);

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wl_seat_send_capabilities(res, caps);

    if (v >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(res, "Wine-Virtual-Seat");
    }

    self->seatResource_ = res;
    OH_LOG_INFO(LOG_APP, "[Seat] client bound v=%{public}u caps=0x%{public}x OK", v, caps);
}

void Seat::seat_get_pointer(wl_client* client, wl_resource* seatRes, uint32_t id) {
    auto* self = static_cast<Seat*>(wl_resource_get_user_data(seatRes));
    uint32_t version = wl_resource_get_version(seatRes);

    wl_resource* ptr = wl_resource_create(client, &wl_pointer_interface, version, id);
    wl_resource_set_implementation(ptr, &kPointerImpl, self, Seat::pointer_destroy);
    {
        std::lock_guard<std::mutex> lk(self->ptrResMutex_);
        self->pointerResources_.push_back(ptr);
        self->ptrCount_.store((int)self->pointerResources_.size());
    }
    OH_LOG_INFO(LOG_APP, "[Seat] wl_pointer created OK (total=%{public}d)", self->ptrCount_.load());
}

void Seat::seat_get_keyboard(wl_client* client, wl_resource* seatRes, uint32_t id) {
    auto* self = static_cast<Seat*>(wl_resource_get_user_data(seatRes));
    uint32_t version = wl_resource_get_version(seatRes);

    wl_resource* kbd = wl_resource_create(client, &wl_keyboard_interface, version, id);
    wl_resource_set_implementation(kbd, &kKeyboardImpl, self, Seat::keyboard_destroy);
    {
        std::lock_guard<std::mutex> lk(self->kbdResMutex_);
        self->keyboardResources_.push_back(kbd);
        self->kbdCount_.store((int)self->keyboardResources_.size());
    }

    // XKB_V1 keymap: 通过 shm_open 传递 xkb keymap fd 给 Wine
    const char* shm_name = "/honwine_keymap";
    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0644);
    if (fd >= 0) {
        shm_unlink(shm_name);
        if (ftruncate(fd, _tmp_keymap_xkb_len) == 0) {
            void* map = mmap(nullptr, _tmp_keymap_xkb_len, PROT_WRITE, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED) {
                memcpy(map, _tmp_keymap_xkb, _tmp_keymap_xkb_len);
                munmap(map, _tmp_keymap_xkb_len);
                wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, _tmp_keymap_xkb_len);
                close(fd);
                OH_LOG_INFO(LOG_APP, "[Seat] wl_keyboard OK (keymap=XKB_V1 len=%{public}u, total=%{public}d)",
                            _tmp_keymap_xkb_len, self->kbdCount_.load());
            } else {
                OH_LOG_ERROR(LOG_APP, "[Seat] wl_keyboard mmap failed, fallback to NO_KEYMAP");
                close(fd);
                wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, 0, 0);
            }
        } else {
            OH_LOG_ERROR(LOG_APP, "[Seat] wl_keyboard ftruncate failed, fallback to NO_KEYMAP");
            close(fd);
            wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, 0, 0);
        }
    } else {
        OH_LOG_ERROR(LOG_APP, "[Seat] wl_keyboard shm_open failed (errno=%{public}d), fallback to NO_KEYMAP", errno);
        wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, 0, 0);
    }
    wl_keyboard_send_repeat_info(kbd, 40, 400);
}

void Seat::seat_get_touch(wl_client* client, wl_resource* seatRes, uint32_t id) {
    uint32_t version = wl_resource_get_version(seatRes);
    wl_resource* tch = wl_resource_create(client, &wl_touch_interface, version, id);
    wl_resource_set_implementation(tch, &kTouchImpl, nullptr, nullptr);
    OH_LOG_INFO(LOG_APP, "[Seat] wl_touch created (unsupported, use pointer)");
}

void Seat::seat_release(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

// -- resource destructors --
void Seat::pointer_destroy(wl_resource* r) {
    auto* self = static_cast<Seat*>(wl_resource_get_user_data(r));
    {
        std::lock_guard<std::mutex> lk(self->ptrResMutex_);
        auto& v = self->pointerResources_;
        v.erase(std::remove(v.begin(), v.end(), r), v.end());
        self->ptrCount_.store((int)v.size());
    }
    // 通知 InputManager: pointer resource 销毁, 重置 focus 状态
    if (self->ptrCount_.load() == 0) {
        InputManager::GetInstance()->ResetPointerEnter();
    }
    OH_LOG_INFO(LOG_APP, "[Seat] wl_pointer destroyed (remaining=%{public}d)", self->ptrCount_.load());
}

void Seat::keyboard_destroy(wl_resource* r) {
    auto* self = static_cast<Seat*>(wl_resource_get_user_data(r));
    {
        std::lock_guard<std::mutex> lk(self->kbdResMutex_);
        auto& v = self->keyboardResources_;
        v.erase(std::remove(v.begin(), v.end(), r), v.end());
        self->kbdCount_.store((int)v.size());
    }
    // 通知 InputManager: keyboard resource 销毁, 重置 enter 状态
    // 下次按键时会重新发送 enter (因为 keyboardEntered_ = false)
    if (self->kbdCount_.load() == 0) {
        InputManager::GetInstance()->ResetKeyboardEnter();
    }
    OH_LOG_INFO(LOG_APP, "[Seat] wl_keyboard destroyed (remaining=%{public}d)", self->kbdCount_.load());
}

// ========================================================================
//  Keycode 映射 (保留向后兼容)
// ========================================================================

uint32_t Seat::MapKeycode(int32_t ohos) {
    switch (ohos) {
        // -- 数字行 --
        case 2000: return 11;   case 2001: return 2;    case 2002: return 3;
        case 2003: return 4;    case 2004: return 5;    case 2005: return 6;
        case 2006: return 7;    case 2007: return 8;    case 2008: return 9;
        case 2009: return 10;

        // -- 方向键 --
        case 2012: return 103;  case 2013: return 108;
        case 2014: return 105;  case 2015: return 106;

        // -- 字母 A-Z --
        case 2017: return 30;   case 2018: return 48;   case 2019: return 46;
        case 2020: return 32;   case 2021: return 18;   case 2022: return 33;
        case 2023: return 34;   case 2024: return 35;   case 2025: return 23;
        case 2026: return 36;   case 2027: return 37;   case 2028: return 38;
        case 2029: return 50;   case 2030: return 49;   case 2031: return 24;
        case 2032: return 25;   case 2033: return 16;   case 2034: return 19;
        case 2035: return 31;   case 2036: return 20;   case 2037: return 22;
        case 2038: return 47;   case 2039: return 17;   case 2040: return 45;
        case 2041: return 21;   case 2042: return 44;

        // -- 标点 --
        case 2043: return 51;   case 2044: return 52;   case 2056: return 41;
        case 2057: return 12;   case 2058: return 13;   case 2059: return 26;
        case 2060: return 27;   case 2061: return 43;   case 2062: return 39;
        case 2063: return 40;   case 2064: return 53;

        // -- 修饰键 --
        case 2045: return 56;   case 2046: return 100;  case 2047: return 42;
        case 2048: return 54;   case 2072: return 29;   case 2073: return 97;
        case 2074: return 58;   case 2076: return 125;  case 2077: return 126;

        // -- 常用键 --
        case 2049: return 15;   case 2050: return 57;   case 2054: return 28;
        case 2070: return 1;    case 2055: return 14;   case 2071: return 111;
        case 2067: return 139;

        // -- 导航键 --
        case 2068: return 104;  case 2069: return 109;
        case 2081: return 102;  case 2082: return 107;
        case 2083: return 110;  case 2052: return 150;
        case 2053: return 155;  case 2084: return 159;

        // -- F1-F12 --
        case 2090: return 59;   case 2091: return 60;   case 2092: return 61;
        case 2093: return 62;   case 2094: return 63;   case 2095: return 64;
        case 2096: return 65;   case 2097: return 66;   case 2098: return 67;
        case 2099: return 68;   case 2100: return 87;   case 2101: return 88;

        // -- 小键盘 --
        case 2102: return 69;   case 2103: return 82;   case 2104: return 79;
        case 2105: return 80;   case 2106: return 81;   case 2107: return 75;
        case 2108: return 76;   case 2109: return 77;   case 2110: return 71;
        case 2111: return 72;   case 2112: return 73;   case 2113: return 98;
        case 2114: return 55;   case 2115: return 74;   case 2116: return 78;
        case 2117: return 83;   case 2119: return 96;

        // -- 媒体键 --
        case 2085: return 207;  case 2086: return 119;
        case 2087: return 128;  case 2089: return 167;

        // -- 系统键 --
        case 2051: return 99;

        default: return 0;
    }
}
