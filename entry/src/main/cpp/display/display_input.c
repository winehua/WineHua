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
#include <wlr/types/wlr_data_device.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
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
static bool g_script_enabled; /* 真机门自动注入脚本 (默认关, smoke 开启) */

/* 焦点 surface 的存活绑定: client (Xwayland) 销毁 wl_surface 时 wlroots
 * 先发 events.destroy 再释放内存 —— 监听自清指针。不清的话
 * wl_ohos_surface_has_content(g_kbd_focus) 就是对已释放内存的读 (T5 t5t
 * 实测: 对话框 surface 销毁后旧焦点悬垂, EnsureKeyboardFocus 因此永不
 * 重选)。监听挂在指针所有者层 (本文件), 不借 ohos_output 的 xs 生命
 * 周期事件 —— xs destroy 与 wl_surface destroy 是两个信号, 时序分离。
 * kbd/ptr 各一个 listener: 同一 surface 可能同时是两种焦点, 而
 * wl_listener 同一时刻只能挂进一个链。
 * 摘链纪律: destroy 发射期内 wl_list_remove 安全 (发射后 surface 连同
 * 链表头一起释放, 之后再摘 = UAF) —— handler 内自摘是 wlroots 惯例。 */
static bool g_kbd_focus_tracked;
static bool g_ptr_focus_tracked;

static void HandleKbdFocusDestroy(struct wl_listener *listener, void *data)
{
    (void)data;
    wl_list_remove(&listener->link);
    g_kbd_focus = NULL;
    g_kbd_focus_tracked = false;
    OHLOG("kbd focus surface destroyed (pointer cleared)");
}

static void HandlePtrFocusDestroy(struct wl_listener *listener, void *data)
{
    (void)data;
    wl_list_remove(&listener->link);
    g_ptr_focus = NULL;
    g_ptr_focus_tracked = false;
}

static struct wl_listener g_kbd_focus_destroy = {
    .notify = HandleKbdFocusDestroy,
};
static struct wl_listener g_ptr_focus_destroy = {
    .notify = HandlePtrFocusDestroy,
};

static void TrackKbdFocus(struct wlr_surface *surf)
{
    if (g_kbd_focus == surf)
        return;
    if (g_kbd_focus_tracked)
    {
        wl_list_remove(&g_kbd_focus_destroy.link);
        g_kbd_focus_tracked = false;
    }
    g_kbd_focus = surf;
    if (surf)
    {
        wl_signal_add(&surf->events.destroy, &g_kbd_focus_destroy);
        g_kbd_focus_tracked = true;
    }
}

static void TrackPtrFocus(struct wlr_surface *surf)
{
    if (g_ptr_focus == surf)
        return;
    if (g_ptr_focus_tracked)
    {
        wl_list_remove(&g_ptr_focus_destroy.link);
        g_ptr_focus_tracked = false;
    }
    g_ptr_focus = surf;
    if (surf)
    {
        wl_signal_add(&surf->events.destroy, &g_ptr_focus_destroy);
        g_ptr_focus_tracked = true;
    }
}

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
    TrackKbdFocus(xs->surface);
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
            TrackPtrFocus(NULL);
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
    /* D9 拖拽排查 (2026-10-05): notepad 4.6s 按住仅收到 1 条 MotionNotify,
     * ArkTS/队列/drain 全通 —— 断点在本函数的 hit-test 丢弃还是 seat 投递,
     * 无法从既有日志判读。每 100 条采样一次命中/丢弃两端。 */
    static unsigned hit_seq;
    bool sampled = (++hit_seq % 100 == 1);
    if (!wl_ohos_surface_has_content(xs ? xs->surface : NULL))
    {
        if (sampled)
            OHLOG("motion DROP at (%{public}d,%{public}d) xs=%{public}p frame=%{public}dx%{public}d",
                  fx, fy, (void *)xs, fw, fh);
        return;
    }
    double sx = (double)(fx - xs->x);
    double sy = (double)(fy - xs->y);
    if (xs->surface != g_ptr_focus)
    {
        /* 悬停即激活 (M1 简化模型): 键盘焦点随指针命中窗口走 */
        FocusClient(xs);
        if (g_display)
            wl_display_flush_clients(g_display); /* 焦点批先行 (同键纪律) */
        wlr_seat_pointer_notify_enter(g_seat, xs->surface, sx, sy);
        TrackPtrFocus(xs->surface);
        OHLOG("pointer enter xs=%{public}p @%{public}d,%{public}d",
              (void *)xs, xs->x, xs->y);
    }
    wlr_seat_pointer_notify_motion(g_seat, NowMsec(), sx, sy);
    /* wl_pointer.frame 必发 (D9, 2026-10-05 实测): Xwayland 的绝对指针输入
     * 按 frame 批量派发 —— motion 入队, frame 才译成 X MotionNotify。此前
     * 从不发 frame, 拖拽的 motion 流全程滞留 (notepad 4.6s 按住仅 1 条
     * MotionNotify, 还是 button 前置 flush 冲出来的), 拖拽永不动。逐条
     * motion+frame 协议开销可接受 (人手 ~60-120 事件/s), 正确性优先。 */
    wlr_seat_pointer_notify_frame(g_seat);
    if (sampled)
        OHLOG("motion hit xs=%{public}p win %{public}d,%{public}d %{public}ux%{public}u "
              "at (%{public}d,%{public}d) local (%{public}.0f,%{public}.0f) focus_kept=%{public}d",
              (void *)xs, xs->x, xs->y, xs->width, xs->height,
              fx, fy, sx, sy, xs->surface == g_ptr_focus);
}

/* 脉冲拉伸 (D15, 对齐 wayland 路线 input_manager 的同款机制
 * docs/architecture/input.md「脉冲拉伸」): 触屏合成点击的按下/抬起同毫秒
 * 到达, wine 抬起沿驱动的路径在 0ms 脉冲下丢效 —— 2026-10-06 实测: 同一
 * wire 序列 (enter→press→release), 开始钮/弹窗菜单项可用, 而对话框按钮/
 * 菜单栏下拉/右键菜单全哑; swipe 把间隔拉开后同一目标立即可用 —— 间隔
 * 即判据。抬起间隔 <100ms 的点击经 one-shot 定时器延迟抬起补足 100ms,
 * 定时器回调自带 frame+flush (顺序: press, [≥100ms], release)。 */
#define PULSE_MIN_MS 100
static int64_t g_pulse_press_ms;
static uint32_t g_pulse_pending_btn;
static struct wl_event_source *g_pulse_release_timer;

static int DeliverPulseRelease(void *data)
{
    (void)data;
    if (!g_seat || !g_pulse_pending_btn)
        return 0;
    wlr_seat_pointer_notify_button(g_seat, NowMsec(), g_pulse_pending_btn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(g_seat);
    if (g_display)
        wl_display_flush_clients(g_display);
    g_pulse_pending_btn = 0;
    return 0; /* one-shot */
}

/* 按钮注入 (M3a-T4): button = evdev 按钮码 (BTN_LEFT 0x110 / BTN_RIGHT
 * 0x111 / BTN_MIDDLE 0x112, linux/input-event-codes.h), 与键注入同口径 ——
 * wlr_seat_pointer_notify_button 的 button 逐字上 wire (wlr_seat_pointer.c
 * 无偏移), libinput 后端同款直传 evdev 码 (backend/libinput/cursor.c),
 * Xwayland 侧按 X 按钮映射表换算。投递前提 = pointer enter 已建立 (motion
 * 先行; 无焦点时丢弃, 与键注入的"无 keyboard 焦点"同语义)。 */
void display_input_inject_button(uint32_t button, bool press)
{
    if (!g_seat)
        return;
    if (!g_ptr_focus)
    {
        static int said;
        if (++said == 1)
            OH_LOG_ERROR(LOG_APP, "inject_button: 无 pointer 焦点 (motion 未先行?)");
        return;
    }
    OHLOG("inject button evdev=0x%{public}x %{public}s", button,
          press ? "press" : "release");
    if (press)
    {
        /* 新按下到来而上一发拉伸抬起还没出: 先立即补发, 保 down/up 配对 */
        if (g_pulse_pending_btn)
            DeliverPulseRelease(NULL);
        wlr_seat_pointer_notify_button(g_seat, NowMsec(), button,
                                       WL_POINTER_BUTTON_STATE_PRESSED);
        /* wl_pointer.frame 必发 (D15, 2026-10-06 用户实测): button 与 motion
         * 同受 Xwayland 的 frame 批量派发 —— 不发 frame 就滞留队列, 到下一
         * 拍 frame (常来自之后的第一条 motion) 才冲出, 双击两拍的事件时间
         * 被压扁/错位。motion 侧同款修复见 D9 注释。 */
        wlr_seat_pointer_notify_frame(g_seat);
        g_pulse_press_ms = NowMsec();
        g_pulse_pending_btn = button;
        if (g_display)
            wl_display_flush_clients(g_display);
        return;
    }
    /* 抬起: 距上次按下不足 PULSE_MIN_MS 则经定时器补足 (无定时器 = 链未全
     * 建, 退回直发)。timer 路径的 frame+flush 在回调里。 */
    if (g_pulse_release_timer)
    {
        int64_t wait = PULSE_MIN_MS - (NowMsec() - g_pulse_press_ms);
        if (wait > 0)
        {
            g_pulse_pending_btn = button;
            wl_event_source_timer_update(g_pulse_release_timer, (int)wait);
            return;
        }
    }
    wlr_seat_pointer_notify_button(g_seat, NowMsec(), button,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(g_seat);
    g_pulse_pending_btn = 0;
    if (g_display)
        wl_display_flush_clients(g_display);
}

/* 轴注入 (D15 手势层): 双指滚动/物理滚轮 → wl_pointer.axis discrete。
 * which 0=纵向 1=横向 (WL_POINTER_AXIS_* 枚举同序), steps = ±N。wire 语义
 * 实读源码钉死: Xwayland 把 discrete 累成 v120 (xwayland-input.c:901,
 * scroll_dy_v120 = 120*discrete), dispatch_scroll_motion 折回 valuator
 * (v120/120 = discrete), DIX emulate_scroll_button_events (getevents.c)
 * 按增量符号出 Button4/5 —— 滚轴 increment=+1.0 (xwayland-input.c:218)
 * ⇒ discrete>0 → Button5 (滚轮向下, winex11 → WM_MOUSEWHEEL 负值)。
 * 与 wayland 分支「向上=正=向下滚」同号, 手势层可直接复用同一 accum
 * 符号约定。value 只喂平滑滚动路径 (winex11 不消费), 取 libinput 滚轮
 * 惯例 10/步。frame 必发, 同 button 注释 (D15)。 */
void display_input_inject_axis(int which, int steps)
{
    if (!g_seat)
        return;
    if (!g_ptr_focus)
    {
        static int said;
        if (++said == 1)
            OH_LOG_ERROR(LOG_APP, "inject_axis: 无 pointer 焦点 (motion 未先行?)");
        return;
    }
    if (steps == 0)
        return;
    OHLOG("inject axis which=%{public}d steps=%{public}d", which, steps);
    wlr_seat_pointer_notify_axis(g_seat, NowMsec(),
                                 which == 1 ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
                                            : WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 (double)steps * 10.0, steps,
                                 WL_POINTER_AXIS_SOURCE_WHEEL,
                                 WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    wlr_seat_pointer_notify_frame(g_seat);
    if (g_display)
        wl_display_flush_clients(g_display);
}

/* ── 跨线程注入投递 (NAPI/ArkTS 线程 → 合成循环线程) ─────────────────────
 * wlr_seat 无锁, 注入必须落在事件循环线程。ArkTS 侧入口 (smoke_napi) 走
 * 队列: 加锁入队 + 管道写 1 字节唤醒循环, 循环侧 fd 回调清队逐条执行
 * 真实注入。管道两端 O_NONBLOCK —— 与 retrigger 管道同坑: libwayland 不
 * 给新增 fd 设非阻塞, 阻塞读会冻死循环线程 (T5 t5k 实测)。唤醒字节写
 * 失败 (EAGAIN=管道满) 无害: 说明已有未消费的唤醒字节, drain 一定会跑。
 * 队列上限 64, 超限丢最旧并记日志 (注入不保证不丢, 但不阻塞 UI 线程)。 */
#define INJECT_QUEUE_CAP 64

struct inject_item
{
    bool is_key;
    bool is_button;
    bool is_axis;      /* D15: 轴注入 (which/Steps 复用 axis 字段) */
    bool is_clipboard; /* D19: text 所有权随队列移交, drain 时交 Apply 释放 */
    uint32_t keycode; /* is_button 时复用为 evdev 按钮码 */
    bool press;
    float nx, ny;
    int phase;
    int axis_which, axis_steps; /* is_axis: 0=纵向 1=横向, ±N 步 */
    char *text; /* is_clipboard: UTF-8 串 (heap) */
};

static struct inject_item g_inject_queue[INJECT_QUEUE_CAP];
static size_t g_inject_head, g_inject_tail;
static pthread_mutex_t g_inject_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_inject_wake[2] = {-1, -1};

static void InjectQueuePush(const struct inject_item *item)
{
    char b = 1;
    pthread_mutex_lock(&g_inject_mutex);
    {
        size_t next = (g_inject_tail + 1) % INJECT_QUEUE_CAP;
        if (next == g_inject_head)
        {
            g_inject_head = (g_inject_head + 1) % INJECT_QUEUE_CAP;
            OHLOG("inject queue full, dropped oldest");
        }
        g_inject_queue[g_inject_tail] = *item;
        g_inject_tail = next;
    }
    pthread_mutex_unlock(&g_inject_mutex);
    if (g_inject_wake[1] >= 0)
    {
        ssize_t rc = write(g_inject_wake[1], &b, 1);
        (void)rc; /* EAGAIN = 唤醒已在途 */
    }
}

static void ApplySetClipboard(char *text); /* D19 剪贴板桥, 定义在 post 段 */

static int InjectQueueDrain(void *data)
{
    (void)data;
    char buf[64];
    ssize_t n;
    while ((n = read(g_inject_wake[0], buf, sizeof buf)) > 0 || errno == EINTR)
        ;
    for (;;)
    {
        struct inject_item item;
        pthread_mutex_lock(&g_inject_mutex);
        if (g_inject_head == g_inject_tail)
        {
            pthread_mutex_unlock(&g_inject_mutex);
            break;
        }
        item = g_inject_queue[g_inject_head];
        g_inject_head = (g_inject_head + 1) % INJECT_QUEUE_CAP;
        pthread_mutex_unlock(&g_inject_mutex);
        if (item.is_key)
            display_input_inject_key(item.keycode, item.press);
        else if (item.is_button)
            display_input_inject_button(item.keycode, item.press);
        else if (item.is_axis)
            display_input_inject_axis(item.axis_which, item.axis_steps);
        else if (item.is_clipboard)
            ApplySetClipboard(item.text);
        else
        {
            display_input_inject_motion(item.nx, item.ny, item.phase);
            /* D9 拖拽排查: motion 注入全程静默 (只有 enter 才打点), 事件流
             * 断在 ArkTS 还是队列/注入侧无从判读 (2026-10-05 实测: notepad
             * 4.6s 按住仅 1 条 MotionNotify)。每 50 条打一次样。 */
            static unsigned motion_drained;
            if (++motion_drained % 50 == 1)
                OHLOG("inject motion #%{public}u (%.3f,%.3f phase=%{public}d)",
                      motion_drained, item.nx, item.ny, item.phase);
        }
    }
    if (g_display)
        wl_display_flush_clients(g_display);
    return 0;
}

void wl_ohos_input_post_key(uint32_t keycode, bool press)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_key = true;
    item.keycode = keycode;
    item.press = press;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_motion(float nx, float ny, int phase)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_key = false;
    item.nx = nx;
    item.ny = ny;
    item.phase = phase;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_button(uint32_t button, bool press)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_button = true;
    item.keycode = button; /* 按钮码复用 keycode 字段 (见 inject_item 注释) */
    item.press = press;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_axis(int which, int steps)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_axis = true;
    item.axis_which = which;
    item.axis_steps = steps;
    InjectQueuePush(&item);
}

/* ── D19 CJK 剪贴板桥 ───────────────────────────────────────────────
 * x11 路线的 Wine 文本框没有 wayland commit_string 通道 (那是
 * winewayland.drv 的 text-input 协议路径), 中文上屏串也折不成键码 ——
 * 走 X selection: wlr_seat_set_selection 由 xwm 自动桥接为 Xwayland 的
 * CLIPBOARD 所有权, wine 作为 X client Ctrl+V 即得全文 (拼音组合/候选
 * 全在系统输入法内完成, 本桥只承接最终串)。串与 source 都在 loop 线程
 * 操作 (与键注入同线程纪律); send 回调由 xwm 在同一 loop 调, 读
 * g_clip_text 无竞争。 */
static char *g_clip_text;
static uint32_t g_clip_serial;

static void ClipSourceSend(struct wlr_data_source *source,
                           const char *mime_type, int32_t fd)
{
    (void)source;
    OHLOG("clip send: mime=%{public}s fd=%{public}d", mime_type ? mime_type : "(null)", fd);
    if (!g_clip_text || !mime_type || strstr(mime_type, "utf-8") == NULL)
    {
        close(fd);
        return;
    }
    size_t len = strlen(g_clip_text), off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, g_clip_text + off, len - off);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
    close(fd);
}

static const struct wlr_data_source_impl CLIP_SOURCE_IMPL = {
    .send = ClipSourceSend,
    /* accept/destroy 留空: destroy 由 wlroots 兜底 free(source) 并释放
     * mime_types 字符串 (types/data_device/wlr_data_source.c 实测);
     * accept 无需应答; dnd_* 仅拖放用 */
};

static void ApplySetClipboard(char *text) /* loop 线程, 接管 text 所有权 */
{
    if (!g_seat)
    {
        free(text);
        return;
    }
    struct wlr_data_source *src = calloc(1, sizeof(*src));
    if (!src)
    {
        free(text);
        return;
    }
    /* 必须经 init: 它零化结构体并初始化 events.destroy 信号 —— 直接
     * calloc 后手工装配会漏信号初始化, wlr_seat_set_selection 绑定
     * offer 时 wl_list_insert 解引用野列表 (12:18 cppcrash 实证)。 */
    wlr_data_source_init(src, &CLIP_SOURCE_IMPL);
    const char *mimes[] = {"text/plain;charset=utf-8", "text/plain"};
    for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]); i++)
    {
        char **slot = wl_array_add(&src->mime_types, sizeof(char *));
        if (slot)
            *slot = strdup(mimes[i]);
    }
    free(g_clip_text);
    g_clip_text = text; /* send 回调读, 同 loop 线程 */
    g_clip_serial++;
    wlr_seat_set_selection(g_seat, src, g_clip_serial);
    OHLOG("clipboard set (%zu bytes)", strlen(g_clip_text));
}

void wl_ohos_input_post_clipboard(const char *utf8)
{
    if (!utf8)
        return;
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_clipboard = true;
    item.text = strdup(utf8);
    if (!item.text)
        return;
    InjectQueuePush(&item);
}

/* ── T1/T2/T5 真机门自动注入脚本: 定时器驱动确定性输入序列 ──────────────
 * 仅存在于 displayroute smoke 链 (本身就是测试路径, principles #22 不进
 * 产品)。
 *   t=8s  KEY_A → 最上层窗口 (win1) 回显
 *   t=12s motion 到 win2 中心 (悬停切换焦点) + KEY_B → win2 回显
 *   t=16s motion 回 win1 中心 + KEY_C → win1 回显, 且 12~16s 间 win1 无键
 *   t=20s motion 到 notepad 文本区中心 + KEY_H
 *   t=21.5/23/24.5/26s KEY_E/L/L/O → notepad 文本区出现 "HELLO"
 *   (终证判据, 2026-10-04 从 "hi" 扩展)
 * notepad 以**无参数**启动 (载体用例 argvMode=raw/argv=[], 见
 * smoke/suites/displayroute.json): 不再有"把 smoke 参数当文件名"的弹框,
 * 键直达文本区。坐标与 xclient_child mode=2 的窗口摆位耦合 (win1 @60,80
 * 320x240, win2 @380,300 280x200; 帧 800x600), notepad 坐标 = 真机实测几何
 * (874x655 @4,30, r0929092617-t5o) —— 两侧常量成对维护。 */
#define KEY_A 30 /* linux/input-event-codes.h evdev 键码 */
#define KEY_B 48
#define KEY_C 46
#define KEY_E 18
#define KEY_H 35
#define KEY_I 23
#define KEY_L 38
#define KEY_O 24 /* 注意不是 32 —— evdev 32=KEY_D, 2026-10-04 实测打成了 "d" */

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
        wl_event_source_timer_update(g_script_timer, 4000);
        break;
    case 4: /* 悬停 notepad 文本区 (主窗内点 217,190) + KEY_H。显式重选焦点:
             * 双窗口重聚焦覆盖 (焦点指针的销毁自清在 Track*, surface destroy
             * 监听里)。 */
        display_input_inject_motion(217 / 800.0f, 190 / 600.0f, 1);
        FocusClient(wl_ohos_output_client_topmost_at(217, 190));
        if (g_display)
            wl_display_flush_clients(g_display);
        display_input_inject_key(KEY_H, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        wl_event_source_timer_update(g_script_timer, 1500);
        break;
    case 5: /* KEY_E ── HELLO 终证序列 (2026-10-04: 目标链判据从 "hi"
             * 扩为 "HELLO"; 1.5s 间隔避开 §1.5 单槽 pending key 的
             * settle 丢键窗口) */
        display_input_inject_key(KEY_E, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        wl_event_source_timer_update(g_script_timer, 1500);
        break;
    case 6: /* KEY_L */
        display_input_inject_key(KEY_L, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        wl_event_source_timer_update(g_script_timer, 1500);
        break;
    case 7: /* KEY_L */
        display_input_inject_key(KEY_L, true);
        if (g_release_timer)
            wl_event_source_timer_update(g_release_timer, 300);
        wl_event_source_timer_update(g_script_timer, 1500);
        break;
    case 8: /* KEY_O → notepad 文本区出现 "HELLO", 序列结束 */
        display_input_inject_key(KEY_O, true);
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

    /* 跨线程注入的唤醒通道: 建在 loop 线程上 (fd 源必须挂在同一 loop)。
     * 两端 O_NONBLOCK, 理由见 InjectQueueDrain 注释。 */
    if (g_loop && pipe(g_inject_wake) == 0)
    {
        int i;
        for (i = 0; i < 2; ++i)
            fcntl(g_inject_wake[i], F_SETFL, O_NONBLOCK);
        wl_event_loop_add_fd(g_loop, g_inject_wake[0], WL_EVENT_READABLE,
                             InjectQueueDrain, NULL);
    }
    else
    {
        OH_LOG_ERROR(LOG_APP, "inject wake pipe create failed errno=%{public}d",
                     errno);
    }

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

    /* 真机门自动注入: T2 脚本序列 (case 1 即 T1 的 KEY_A 步)。
     * 门控 = wl_ohos_input_set_script_enabled (默认关): 编排是测试资产
     * (定时重放按键 + 硬编码窗口几何), 不经开启就重放 = 产品路径里藏
     * 幽灵输入 (原则 #23: 定制默认关闭、关闭态 = 无此行为)。手动注入
     * (队列/手动键) 不受此门影响。 */
    if (g_loop)
    {
        g_release_timer = wl_event_loop_add_timer(g_loop, KeyReleaseTick, NULL);
        g_key_timer = wl_event_loop_add_timer(g_loop, DeliverPendingKey,
                                              &g_pending_key);
        g_pulse_release_timer = wl_event_loop_add_timer(g_loop,
                                                        DeliverPulseRelease,
                                                        NULL);
        if (g_script_enabled)
        {
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

void wl_ohos_input_script_restart(void)
{
    if (!g_loop)
        return;
    /* 建链时 script_enabled=false (产品桌面页先到抢建) 而重触发翻转为
     * true 的路径: seat_create 没建脚本定时器, 这里必须补建 —— 否则
     * restart 静默 no-op 而调用方照打 "re-armed" (2026-10-05 hello 实测:
     * inject key 0 条, 注入脚本一次没跑, HELLO 终证落空)。 */
    if (!g_script_timer && g_script_enabled)
        g_script_timer = wl_event_loop_add_timer(g_loop, ScriptTick, NULL);
    if (!g_script_timer)
        return;
    g_script_step = 0;
    wl_event_source_timer_update(g_script_timer, 8000);
    OHLOG("inject script re-armed (t=8/12/16/20/20.5/21s)");
}

void wl_ohos_input_set_script_enabled(bool enabled)
{
    /* 只在 seat_create 前生效 (开启态决定定时器是否武装); 链建后再翻转
     * 不追认 —— 显示路线每轮触发走完整 seat 生命周期 (retrigger 重进)。 */
    g_script_enabled = enabled;
}
