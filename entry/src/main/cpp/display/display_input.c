/*
 * display_input.c — M1-T1 displayroute 输入链
 *
 * wlr_seat + 虚拟键盘 + OHOS 事件注入桥。链路:
 *   wlr_seat_create → wlr_seat_set_capabilities (P|K, 缺它 Xwayland 不绑
 *   keyboard/pointer) → 虚拟键盘 wlr_keyboard_init + set_keymap (evdev/us,
 *   数据 = wine-data/xkb) → wlr_seat_set_keyboard → (xwm 建立后)
 *   wlr_xwayland_set_seat
 *
 * 注入语义 (全部实读 wlroots 0.20.2 源码核对):
 *   - wlr_seat_keyboard_notify_key 的 key 逐字上 wire (wlr_seat_keyboard.c:81
 *     无偏移), libinput 后端同款直传 evdev 码 (backend/libinput/keyboard.c:52),
 *     Xwayland 侧自行 +8 对 XKB keymap —— 注入 API 收 evdev 键码。
 *   - keymap 到客户端的通道 = seat 上的 wlr_keyboard (wlr_seat_keyboard.c:125
 *     needs_keymap_update 读 keyboard->keymap), 无真实输入设备时虚拟键盘
 *     (静态 wlr_keyboard + 空实现) 是正规做法。
 *   - 焦点: wl_keyboard.enter 未发前客户端不收 key; 每次 inject 前对当前
 *     client surface 补 keyboard enter (零修饰符)。
 */
#define WLR_USE_UNSTABLE
#include "display_input.h"
#include "ohos_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <hilog/log.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h> /* WL_SEAT_CAPABILITY_* / WL_KEYBOARD_KEY_STATE_* */
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_compositor.h> /* struct wlr_surface 完整定义 */
#include <wlr/types/wlr_keyboard.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/xwayland/xwayland.h>

#define LOG_TAG "display-input"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)

/* 虚拟键盘实现: 无后端输入设备背书, 全部方法为空 (led_update 无 LED 可更) */
static const struct wlr_keyboard_impl k_kb_impl = {
    .name = "displayroute-vkbd",
    .led_update = NULL,
};

static struct wlr_seat *g_seat;
static struct wlr_keyboard g_kb;
static struct wl_event_loop *g_loop;
static struct wlr_surface *g_kbd_focus; /* 已 keyboard enter 的 surface */
static struct wlr_surface *g_ptr_focus; /* 已 pointer enter 的 surface */
static struct wl_event_source *g_press_timer;
static struct wl_event_source *g_release_timer;

static uint32_t NowMsec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* keymap 编译: evdev 规则 + pc105 + us 布局, 数据树 = wine-data/xkb
 * (assemble.sh:596 断言 rules/evdev 后打包, 设备解压到 files/wine/xkb)。
 * libxkbcommon 的默认 include 前缀是主机路径, 设备不存在, 必须显式指路;
 * setenv 放在 context/keymap 创建之前 (xkbcommon 按环境变量现读现用)。 */
static struct xkb_keymap *BuildKeymap(void)
{
    const char *root = "/data/storage/el2/base/files/wine/xkb";
    if (access(root, R_OK) != 0)
        OH_LOG_ERROR(LOG_APP, "xkb data missing: %{public}s (wine-data 未解压?)", root);
    setenv("XKB_CONFIG_ROOT", root, 0); /* 不覆盖调用方已设值 */

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx)
        return NULL;
    struct xkb_rule_names names = {
        .rules = "evdev",
        .model = "pc105",
        .layout = "us",
    };
    struct xkb_keymap *km =
        xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km)
        OH_LOG_ERROR(LOG_APP, "xkb_keymap_new_from_names failed (XKB_CONFIG_ROOT=%{public}s)",
                     getenv("XKB_CONFIG_ROOT") ? getenv("XKB_CONFIG_ROOT") : "(unset)");
    /* keymap 持有自己的 context 引用, unref 上下文安全 */
    xkb_context_unref(ctx);
    return km;
}

static void EnsureKeyboardFocus(void)
{
    struct wlr_xwayland_surface *xs = wl_ohos_output_client_xs();
    struct wlr_surface *surf = xs ? xs->surface : NULL;
    if (!surf || !surf->mapped || surf == g_kbd_focus)
        return;
    /* 激活先行: wlr_xwayland_surface_activate → xwm_focus_window →
     * xcb_set_input_focus。不调它 X server 输入焦点停在 PointerRoot,
     * DIX 的 KeyPress 无投递目标 (gate5 实测: seat/keymap/enter 全通,
     * X client 无回显)。T2 起按点按窗口激活, leave 时 activate(false)。 */
    wlr_xwayland_surface_activate(xs, true);
    uint32_t keycodes[1] = {0};
    struct wlr_keyboard_modifiers mods = {0};
    wlr_seat_keyboard_notify_enter(g_seat, surf, keycodes, 0, &mods);
    g_kbd_focus = surf;
    OHLOG("keyboard enter surf=%{public}p (activated)", (void *)surf);
}

void display_input_inject_key(uint32_t keycode, bool press)
{
    if (!g_seat)
        return;
    EnsureKeyboardFocus();
    if (!g_kbd_focus)
    {
        static int said;
        if (++said == 1)
            OH_LOG_ERROR(LOG_APP, "inject_key: 无 keyboard 焦点 (client 未 map?)");
        return;
    }
    OHLOG("inject key evdev=%{public}u %{public}s", keycode,
          press ? "press" : "release");
    wlr_seat_keyboard_notify_key(g_seat, NowMsec(), keycode,
                                 press ? WL_KEYBOARD_KEY_STATE_PRESSED
                                       : WL_KEYBOARD_KEY_STATE_RELEASED);
}

void display_input_inject_motion(float nx, float ny, int phase)
{
    if (!g_seat)
        return;
    if (phase == 2)
    {
        if (g_ptr_focus)
        {
            wlr_seat_pointer_notify_clear_focus(g_seat);
            g_ptr_focus = NULL;
        }
        return;
    }
    struct wlr_surface *surf = wl_ohos_output_client_xs() != NULL
                                   ? wl_ohos_output_client_xs()->surface
                                   : NULL;
    if (!surf || !surf->mapped)
        return;
    double sx = (double)nx * surf->current.width;
    double sy = (double)ny * surf->current.height;
    if (surf != g_ptr_focus)
    {
        wlr_seat_pointer_notify_enter(g_seat, surf, sx, sy);
        g_ptr_focus = surf;
        OHLOG("pointer enter surf=%{public}p", (void *)surf);
    }
    wlr_seat_pointer_notify_motion(g_seat, NowMsec(), sx, sy);
}

/* ── T1 真机门自动注入: 8s 后 KEY_A press, +300ms release ────────────────
 * 仅存在于 displayroute smoke 链 (本身就是测试路径, principles #22 不进产品)。
 * 判据 = XCLIENT-NCP 的 key 回显; T5 编排化后由 NAPI 驱动替代本定时器。 */
#define KEY_A 30 /* linux/input-event-codes.h evdev 键码 */

static int KeyReleaseTick(void *data)
{
    (void)data;
    display_input_inject_key(KEY_A, false);
    return 0; /* 不重排 = one-shot */
}

static int KeyPressTick(void *data)
{
    (void)data;
    display_input_inject_key(KEY_A, true);
    if (g_release_timer)
        wl_event_source_timer_update(g_release_timer, 300);
    return 0;
}

int wl_ohos_input_seat_create(struct wl_display *wl, struct wl_event_loop *loop)
{
    g_loop = loop;

    struct xkb_keymap *km = BuildKeymap();
    wlr_keyboard_init(&g_kb, &k_kb_impl, "displayroute-vkbd");
    if (km && !wlr_keyboard_set_keymap(&g_kb, km))
        OH_LOG_ERROR(LOG_APP, "wlr_keyboard_set_keymap failed");

    g_seat = wlr_seat_create(wl, "default");
    if (!g_seat)
    {
        OH_LOG_ERROR(LOG_APP, "wlr_seat_create failed");
        return -1;
    }
    /* capabilities 缺省为 0: 客户端按掩码决定是否 bind keyboard/pointer,
     * 不设则 Xwayland 只绑 seat 本体 (wl_keyboard 永远不来) */
    wlr_seat_set_capabilities(g_seat,
                              WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    wlr_seat_set_keyboard(g_seat, &g_kb);

    /* T1 真机门自动注入定时器 */
    if (g_loop)
    {
        g_press_timer = wl_event_loop_add_timer(g_loop, KeyPressTick, NULL);
        g_release_timer = wl_event_loop_add_timer(g_loop, KeyReleaseTick, NULL);
        if (g_press_timer && g_release_timer)
            wl_event_source_timer_update(g_press_timer, 8000);
    }
    OHLOG("seat up (keymap=%{public}s, auto-inject 'a' @8s)",
          g_kb.keymap ? "ok" : "MISSING");
    return 0;
}

void wl_ohos_input_xwayland_set_seat(struct wlr_xwayland *xwayland)
{
    if (!g_seat || !xwayland)
        return;
    wlr_xwayland_set_seat(xwayland, g_seat);
    OHLOG("xwayland seat attached");
}
