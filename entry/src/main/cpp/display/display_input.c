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
static uint32_t g_held_keycode; /* 已按下待释放的键 (release 定时器目标) */
static struct wl_display *g_display; /* 键/焦点批次的显式 flush 用 */
static int64_t g_focus_switched_ms; /* 最近一次焦点切换时刻 (settle 判定) */

/* 焦点切换后按键的 settle 窗口: enter 与 key 写入同一 wl 连接缓冲会被
 * 对端一次 read 同时收达, Xwayland 先派发 wayland 事件 (enter+key) 后
 * 处理 X 请求 (SetInputFocus), press 落旧焦点 (T2 实测)。settle 期内的
 * 首个键击延后发送, 让焦点消息先被消费; 后续键击零延迟。 */
#define FOCUS_SETTLE_MS 40

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

static void FocusClient(struct wlr_xwayland_surface *xs)
{
    if (!wl_ohos_surface_has_content(xs ? xs->surface : NULL))
        return;
    if (xs->surface == g_kbd_focus)
        return;
    /* 激活先行: wlr_xwayland_surface_activate → xwm_focus_window →
     * xcb_set_input_focus。不调它 X server 输入焦点停在 PointerRoot,
     * DIX 的 KeyPress 无投递目标 (gate5 实测: seat/keymap/enter 全通,
     * X client 无回显)。M1 激活模型 = 指针命中即切换 (T2); leave 时
     * activate(false) 随焦点转移隐式完成 (新窗 activate 覆盖旧窗)。 */
    wlr_xwayland_surface_activate(xs, true);
    uint32_t keycodes[1] = {0};
    struct wlr_keyboard_modifiers mods = {0};
    wlr_seat_keyboard_notify_enter(g_seat, xs->surface, keycodes, 0, &mods);
    g_kbd_focus = xs->surface;
    g_focus_switched_ms = NowMsec();
    OHLOG("keyboard enter xs=%{public}p surf=%{public}p (activated)",
          (void *)xs, (void *)xs->surface);
}

static void EnsureKeyboardFocus(void)
{
    /* 已有有效焦点时不抢: inject_key 的职责是投递, 不重选焦点窗口
     * (T2 实测: 每次注入都拉回最上层窗口, 会把 motion 选中的焦点
     * 翻回去 —— 同毫秒 keyboard enter 双跳)。 */
    if (wl_ohos_surface_has_content(g_kbd_focus))
        return;
    FocusClient(wl_ohos_output_client_xs());
}

/* 键投递的顺序纪律: 焦点切换 (activate → xwm SetInputFocus + wl enter)
 * 的消息必须被对端消费后才发 key —— enter 与 key 同批到达时, Xwayland
 * 先派发 wayland 事件后处理 X 请求, press 按旧焦点投递 (T2 实测)。
 * 措施: 焦点切换后 FOCUS_SETTLE_MS 内的首个键击经定时器延后; 其余键击
 * 立即发 (打字节奏零延迟)。显式 flush 括号保证字节尽早出网。 */
struct PendingKey
{
    uint32_t keycode;
    bool press;
};
static struct PendingKey g_pending_key;
static struct wl_event_source *g_key_timer;

static int DeliverPendingKey(void *data)
{
    struct PendingKey *k = data;
    if (!g_seat || !g_kbd_focus)
        return 0;
    wlr_seat_keyboard_notify_key(g_seat, NowMsec(), k->keycode,
                                 k->press ? WL_KEYBOARD_KEY_STATE_PRESSED
                                          : WL_KEYBOARD_KEY_STATE_RELEASED);
    if (g_display)
        wl_display_flush_clients(g_display);
    return 0; /* one-shot */
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
    if (press)
        g_held_keycode = keycode;

    if (g_loop && g_key_timer &&
        (uint32_t)(NowMsec() - g_focus_switched_ms) < FOCUS_SETTLE_MS)
    {
        /* 焦点刚切: 延后一拍发送 (settle) */
        g_pending_key.keycode = keycode;
        g_pending_key.press = press;
        wl_event_source_timer_update(g_key_timer, FOCUS_SETTLE_MS);
        return;
    }
    wlr_seat_keyboard_notify_key(g_seat, NowMsec(), keycode,
                                 press ? WL_KEYBOARD_KEY_STATE_PRESSED
                                       : WL_KEYBOARD_KEY_STATE_RELEASED);
    if (g_display)
        wl_display_flush_clients(g_display);
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
    /* 归一化 (XComponent 局部) → 帧坐标 → 命中窗口 → 窗口局部坐标。
     * 帧尺寸取数口化 (当前 800x600, 与 chain_start 的 output 一致)。 */
    int fw = 800, fh = 600;
    wl_ohos_output_frame_size(&fw, &fh);
    int fx = (int)(nx * (float)fw);
    int fy = (int)(ny * (float)fh);
    struct wlr_xwayland_surface *xs = wl_ohos_output_client_topmost_at(fx, fy);
    if (!wl_ohos_surface_has_content(xs ? xs->surface : NULL))
        return;
    double sx = (double)(fx - xs->x);
    double sy = (double)(fy - xs->y);
    if (xs->surface != g_ptr_focus)
    {
        /* 悬停即激活 (M1 简化模型): 键盘焦点随指针命中窗口走 */
        FocusClient(xs);
        if (g_display)
            wl_display_flush_clients(g_display); /* 焦点批先行 (同键纪律) */
        wlr_seat_pointer_notify_enter(g_seat, xs->surface, sx, sy);
        g_ptr_focus = xs->surface;
        OHLOG("pointer enter xs=%{public}p @%{public}d,%{public}d",
              (void *)xs, xs->x, xs->y);
    }
    wlr_seat_pointer_notify_motion(g_seat, NowMsec(), sx, sy);
}

/* ── T1/T2 真机门自动注入脚本: 定时器驱动确定性输入序列 ────────────────
 * 仅存在于 displayroute smoke 链 (本身就是测试路径, principles #22 不进
 * 产品)。T5 编排化后由 NAPI 驱动替代。
 *   t=8s  KEY_A → 最上层窗口 (win1) 回显
 *   t=12s motion 到 win2 中心 (悬停切换焦点) + KEY_B → win2 回显
 *   t=16s motion 回 win1 中心 + KEY_C → win1 回显, 且 12~16s 间 win1 无键
 * 坐标与 xclient_child mode=2 的窗口摆位耦合 (win1 @60,80 320x240,
 * win2 @380,300 280x200; 帧 800x600) —— 两侧常量成对维护。 */
#define KEY_A 30 /* linux/input-event-codes.h evdev 键码 */
#define KEY_B 48
#define KEY_C 46

static struct wl_event_source *g_script_timer;
static int g_script_step;

static int ScriptTick(void *data)
{
    (void)data;
    ++g_script_step;
    switch (g_script_step)
    {
    case 1: /* 悬停到 win1 中心 + KEY_A: (60+160, 80+120) = (220,200) */
        display_input_inject_motion(220 / 800.0f, 200 / 600.0f, 1);
        display_input_inject_key(KEY_A, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        wl_event_source_timer_update(g_script_timer, 4000);
        break;
    case 2: /* 悬停到 win2 中心: (380+140, 300+100) = (520,400) */
        display_input_inject_motion(520 / 800.0f, 400 / 600.0f, 1);
        display_input_inject_key(KEY_B, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        wl_event_source_timer_update(g_script_timer, 4000);
        break;
    case 3: /* 悬停回 win1 中心 + KEY_C */
        display_input_inject_motion(220 / 800.0f, 200 / 600.0f, 1);
        display_input_inject_key(KEY_C, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        break; /* 序列结束, 不重排 */
    default:
        break;
    }
    return 0;
}

static int KeyReleaseTick(void *data)
{
    (void)data;
    /* 释放实际按下的键: 硬编码 KEY_A 曾让 KEY_B/C 永不释放, X server
     * auto-repeat 以 press/release 对洪水重发 (T2 实测 c×646)。 */
    display_input_inject_key(g_held_keycode, false);
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
    g_display = wl;

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

    /* 真机门自动注入: T2 脚本序列 (case 1 即 T1 的 KEY_A 步) */
    if (g_loop)
    {
        g_release_timer = wl_event_loop_add_timer(g_loop, KeyReleaseTick, NULL);
        g_key_timer = wl_event_loop_add_timer(g_loop, DeliverPendingKey,
                                              &g_pending_key);
        g_script_timer = wl_event_loop_add_timer(g_loop, ScriptTick, NULL);
        if (g_script_timer && g_release_timer)
        {
            wl_event_source_timer_update(g_script_timer, 8000);
        }
        else
        {
            /* 兜底: 脚本定时器建失败退回 T1 单键序列 */
            g_press_timer = wl_event_loop_add_timer(g_loop, KeyPressTick, NULL);
            if (g_press_timer && g_release_timer)
                wl_event_source_timer_update(g_press_timer, 8000);
        }
    }
    OHLOG("seat up (keymap=%{public}s, script @8/12/16s)",
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
