#include "seat.h"
#include "keymap_xkb.h"
#include "input_manager.h"
#include "compositor/wayland_server.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
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
// set_cursor 不处理: 系统光标由 host (ArkTS/OH_WindowManager) 显示, wine 的
// shm 自定义光标不呈现 — 光标可见性跟踪交由相对模式的
// PointerExtras/InputManager (与 wine 侧 cursor.wl_surface 时序无耦合)。
static void ptr_set_cursor(wl_client*, wl_resource*, uint32_t,
                           wl_resource*, int32_t, int32_t) {}
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

    // XKB_V1 keymap: 通过匿名 fd 传递 xkb keymap 给 Wine
    // memfd_create 创建匿名共享内存, 无全局命名空间, 多客户端天然隔离
    int fd = memfd_create("winehua_keymap", MFD_CLOEXEC);
    if (fd >= 0) {
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
        OH_LOG_ERROR(LOG_APP, "[Seat] wl_keyboard memfd_create failed (errno=%{public}d), fallback to NO_KEYMAP", errno);
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
