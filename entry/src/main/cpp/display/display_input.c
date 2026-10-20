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
#include "desktop_fit.h"   /* 游戏全屏适配: letterbox 逆映射 (C 壳, 单一实现) */
#include "ohos_egl_import.h" /* wl_ohos_egl_canvas_size: fit 基准与 present 同源 */
#include "xim_bridge.h"
#include "x11_toplevel.h" /* M4a-T5: mm 注入 drain 分派 (C1: loop 线程) */
#include <wlr/types/wlr_data_device.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>

#include <hilog/log.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h> /* WL_SEAT_CAPABILITY_* / WL_KEYBOARD_KEY_STATE_* */
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
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
/* zwp_relative_pointer_manager_v1 (#177): Xwayland 绑定后把相对增量译成
 * XI2 raw motion (POINTER_RAWONLY + unaccelerated valuators) —— wine
 * dinput 相对模式吃的正是这条流。此前无此 global, 绝对派发不带 NORAW,
 * wine 的「相对」退化为帧空间坐标差 (letterbox 缩放错 + 边缘钳位跳变)
 * = 鼠标飘。增量喂点在 inject_motion。 */
static struct wlr_relative_pointer_manager_v1 *g_rel_manager;
/* 相对增量状态: 仅同 surface 连续 motion 喂 (锚点跳变/跨面不喂 —— 触点
 * 落屏是传送不是移动, 巨幅 raw delta 会让游戏镜头乱转); 幅度超 surface
 * 短边 1/8 视为跳变重置。 */
static struct wlr_surface *g_rel_surf;
static double g_rel_lsx, g_rel_lsy;
static bool g_rel_valid;
/* 最近 motion 命中的 X window id (#139 双击同一性活身份): loop 线程写,
 * 读取线程读 u32 快照 (良性竞争, 见 display_input.h 声明处判据) */
static uint32_t g_last_motion_win;
static struct wlr_keyboard g_kb;
static struct wl_event_loop *g_loop;
static struct wlr_xwayland *g_xwayland; /* GAP2 (#131): X 硬件焦点同步口
                                         * (xwm 连接 + window_id), set_seat 时注入 */
static struct wlr_surface *g_kbd_focus; /* 已 keyboard enter 的 surface */
static struct wlr_surface *g_ptr_focus; /* 已 pointer enter 的 surface */
static struct wlr_xwayland_surface *g_ptr_focus_xs; /* 命中的 xs (D36 点击置前用) */
static struct wl_display *g_display; /* 键/焦点批次的显式 flush 用 */
static int64_t g_focus_switched_ms; /* 最近一次焦点切换时刻 (settle 判定) */

/* 游戏全屏适配 (2026-10-15): 最近一次 motion 是否落在 letterbox 黑边区
 * (画布逻辑系, 由 display_input_inject_motion 按 present 同源 fit 判定)。
 * 语义: 黑边区 = 死区 —— 点击/滚轮吞掉 (用户拍板), motion 钳到内容边缘
 * (保留拖拽/边缘跟随, 防相对模式幽灵增量, 同 wayland 臂 ClampToContent
 * 依据)。吞下的按下按位记账, 其抬起配平同吞; 非吞按下会清同键位残留,
 * 丢抬起不泄漏 (见 button 注入注释)。 */
static int g_ptr_in_letterbox;
static uint32_t g_swallowed_btn_mask;

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

static uint64_t NowUsec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000u);
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

/* GAP2 (#131): X 硬件输入焦点同步 (x11 multimode)。
 *
 * 问题 (真机实测 2026-10-11, .temp/gap2/wine_trace_after_key2.log):
 * `wlr_xwayland_surface_activate` 对 wine 走 WM_TAKE_FOCUS, 而 xwm 的
 * message data32[1] = CurrentTime = 0 (wlroots xwm_focus_window); wine 的
 * 接受门 (winex11 event.c:738 `hwnd != foreground &&
 * !window_should_take_focus(foreground, 0)`, window.c:2146
 * `!focus_time || time > focus_time`, focus_time = 前台窗 WM_STATE 属性
 * 事件时间 ≠ 0) 对 time=0 恒拒绝 —— 实测 wine WARN "Ignoring ...
 * WM_TAKE_FOCUS ... event_time 0, foreground 0x10060" 后 KeyPress 落旧窗。
 * ⇒ 合成器驱动的焦点切换对 wine 结构性不可达; wine 自接受路径
 * (event.c:635 set_focus) 在有 EWMH 时也只做 XSetInputFocus
 * (不动 win32 前台) —— 本同步即该等价动作, 绕过的只是 time=0 门。
 *
 * 纪律: 每次 FocusClient 调用都重断言 (不只在切换时) —— wine 自置前台时
 * 会主动 XSetInputFocus 回自己, X 焦点会漂移, 键注入前必须对齐目标。
 * 门: multimode (x11_toplevel_active, 否则虚拟桌面/wayland 路线零行为
 * 变化) + 非 OR 窗 (菜单等 override-redirect 不进 X 焦点模型) + surface
 * 在册 (withdrawn 窗不碰)。 */
static void SyncXInputFocus(struct wlr_xwayland_surface *xs)
{
    if (!g_xwayland || !xs || !xs->surface || xs->override_redirect)
        return;
    if (!x11_toplevel_active())
        return; /* multimode 门 (上面的行为清单依据) */
    xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(g_xwayland);
    if (!conn)
        return;
    static struct wlr_xwayland_surface *logged;
    if (logged != xs)
    {
        logged = xs;
        OHLOG("x-focus sync xs=%{public}p win=0x%{public}x (multimode)",
              (void *)xs, xs->window_id);
    }
    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, xs->window_id,
                        XCB_CURRENT_TIME);
    xcb_flush(conn); /* 先于后续键注入上位 (settle 40ms 兜竞态) */
}

static void FocusClient(struct wlr_xwayland_surface *xs)
{
    if (!wl_ohos_surface_has_content(xs ? xs->surface : NULL))
        return;
    if (x11_toplevel_modal_blocks(xs)) {
        /* 任务 #167 (WM 职责, EWMH 同构): 目标被模态对话框阻断 (owner) ——
         * 不激活不夺焦。点击照常投递, 由 wine 的禁用窗语义自行丢弃
         * (WM_MOUSEACTIVATE → MA_NOACTIVATEANDEAT); 焦点/激活留在对话框。
         * 重入安全: 判定内部 trylock, popup 持锁路径 fail-open。 */
        OHLOG("focus-blocked xs=%{public}p owns-modal (skip activate+focus)",
              (void *)xs);
        return;
    }
    SyncXInputFocus(xs);
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
        g_ptr_focus_xs = NULL;
        g_rel_valid = false; /* 相对增量流随指针离场断链 (下次 motion 重锚) */
        return;
    }
    /* 归一化 (XComponent 局部) → 画布坐标 → fit 逆映射 → 帧内桌面坐标 →
     * 命中窗口 → 窗口局部坐标。fit 基准 = 画布 surface 实时尺寸 (#172
     * 尾随, 与 ohos_egl_import.c present 正变换同源同参 —— wayland
     * EglRenderer 同语义); 查询失败退回帧尺寸 (= 旧基准, 行为不变)。 */
    int fw = 800, fh = 600;
    wl_ohos_output_frame_size(&fw, &fh);
    int cw = fw, ch = fh;
    wl_ohos_egl_canvas_size(&cw, &ch);
    int fx = (int)(nx * (float)cw);
    int fy = (int)(ny * (float)ch);
    /* 游戏全屏适配 (2026-10-15): 桌面内容 letterbox 逆映射 —— 与
     * ohos_egl_import.c present 的正变换同源 (WineHua_ComputeDesktopFit,
     * 唯一实现)。桌面 == 画布 (恒等) 或桌面矩形无效时原样直通, 与改动前
     * 逐点一致。黑边区: 坐标钳到内容边缘 (不吞 motion —— 钳制保留拖拽/
     * 边缘跟随语义且相对增量不成幽灵, 同 geometry.h ClampToContent 依据);
     * 黑边区的按下/滚轮由 button/axis 侧吞掉。 */
    {
        int dx = 0, dy = 0, dw = 0, dh = 0;
        struct DesktopFitRect fit;
        /* 直通判定用数学恒等 (scale 1 + 零偏移), 不比 dst==canvas 尺寸:
         * 活窗口基准下 dst 可恰等于 canvas 而 scale=2 (桌面 1560 → 窗口
         * 3120), 尺寸相等会误判直通跳过缩放 —— 实测 2026-10-18: 坐标未
         * 除 2, 物理左下角 (6,2040) 直通成 X 系 (6,2040), 落在 1560x1040
         * 桌面之外, topmost 命中失败, 任务栏点击全部丢失。#157 时代
         * canvas==buffer 时两判定等价, 无此坑。 */
        if (wl_ohos_output_desktop_rect(&dx, &dy, &dw, &dh) &&
            WineHua_ComputeDesktopFit(cw, ch, dw, dh, &fit) &&
            (fit.scale != 1.0 || fit.offX != 0 || fit.offY != 0)) {
            g_ptr_in_letterbox = (fx < fit.offX || fx >= fit.offX + fit.dstW ||
                                  fy < fit.offY || fy >= fit.offY + fit.dstH);
            double ux = dx + WineHua_FitUnmapX(&fit, (double)fx);
            double uy = dy + WineHua_FitUnmapY(&fit, (double)fy);
            if (g_ptr_in_letterbox) {
                if (ux < dx) ux = dx;
                if (ux > dx + dw - 1) ux = dx + dw - 1;
                if (uy < dy) uy = dy;
                if (uy > dy + dh - 1) uy = dy + dh - 1;
            }
            fx = (int)lround(ux);
            fy = (int)lround(uy);
        } else {
            g_ptr_in_letterbox = 0;
        }
    }
    struct wlr_xwayland_surface *xs = wl_ohos_output_client_topmost_at(fx, fy);
    /* D9 拖拽排查 (2026-10-05): notepad 4.6s 按住仅收到 1 条 MotionNotify,
     * ArkTS/队列/drain 全通 —— 断点在本函数的 hit-test 丢弃还是 seat 投递,
     * 无法从既有日志判读。每 100 条采样一次命中/丢弃两端。 */
    static unsigned hit_seq;
    bool sampled = (++hit_seq % 100 == 1);
    if (!wl_ohos_surface_has_content(xs ? xs->surface : NULL))
    {
        g_last_motion_win = 0; /* 命中失效: 身份槽归零 (#139 读数不悬挂旧窗) */
        if (sampled)
            OHLOG("motion DROP at (%{public}d,%{public}d) canvas=%{public}dx%{public}d xs=%{public}p frame=%{public}dx%{public}d",
                  fx, fy, cw, ch, (void *)xs, fw, fh);
        return;
    }
    g_last_motion_win = (uint32_t)xs->window_id;
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
    }
    g_ptr_focus_xs = xs; /* D36: 点击置前的目标 (悬停不置前, 按下才置) */
    /* 相对增量 (#177): 同 surface 连续且幅度不超阈才喂 —— 跨面换窗、点按
     * 传送锚 (pressAt 重放/悬停激活) 一律断链重锚, 不产 delta。增量口径 =
     * surface-local px, Xwayland 侧自行乘 global_surface_scale/viewport
     * (xwayland-input.c dispatch_relative_motion)。 */
    if (g_rel_manager && g_rel_valid && xs->surface == g_rel_surf &&
        fabs(sx - g_rel_lsx) <= (double)(xs->width > 0 ? xs->width : 1) / 8.0 &&
        fabs(sy - g_rel_lsy) <= (double)(xs->height > 0 ? xs->height : 1) / 8.0)
    {
        double rdx = sx - g_rel_lsx, rdy = sy - g_rel_lsy;
        wlr_relative_pointer_manager_v1_send_relative_motion(
            g_rel_manager, g_seat, NowUsec(), rdx, rdy, rdx, rdy);
    }
    g_rel_surf = xs->surface;
    g_rel_lsx = sx;
    g_rel_lsy = sy;
    g_rel_valid = true;
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
    /* 游戏全屏适配 (2026-10-15): letterbox 黑边区按下/抬起吞掉 (用户拍板
     * 口径)。按下吞掉记账, 其抬起配平同吞; 游戏内按下的抬起不受位置影响
     * (按下时的归属即意图)。非吞按下清同键位残留 —— 丢抬起的会话残留
     * 不会把后续抬起误吞 (自愈)。 */
    uint32_t bit = (button >= 0x110 && button <= 0x112) ? (1u << (button - 0x110)) : 0u;
    if (bit && g_ptr_in_letterbox)
    {
        if (press)
        {
            g_swallowed_btn_mask |= bit;
            OHLOG("inject button evdev=0x%{public}x press SWALLOWED (letterbox)", button);
            return;
        }
        if (g_swallowed_btn_mask & bit)
        {
            g_swallowed_btn_mask &= ~bit;
            OHLOG("inject button evdev=0x%{public}x release SWALLOWED (letterbox)", button);
            return;
        }
    }
    if (press)
        g_swallowed_btn_mask &= ~bit; /* 非吞按下: 清残留 (见上) */
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
        /* D36 点击置前: 按下即把命中窗口提到最上 (g_clients 链序 + scene
         * 节点序)。悬停只换焦点不换序, 按下才置前 —— 与桌面合成器惯例
         * 一致, 否则点到的窗口画面不动、输入却进去了。 */
        wl_ohos_output_client_raise(g_ptr_focus_xs);
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

/* 轴注入 (D15 手势层): 双指滚动/物理滚轮 → wl_pointer.axis。
 * which 0=纵向 1=横向 (WL_POINTER_AXIS_* 枚举同序), steps = ±N 格。
 * wire 语义按客户端协商版本分叉 (D29 实测, #93 根因): Xwayland 绑
 * wl_seat v8 (xwayland-input.c:1966 seat_version=8) ⇒ wlroots 走
 * axis_value120 (v8+) 分支, value_discrete 按 wire 协议必须已是
 * 1/120 格单位 (1 格 = 120)——按格数直传会被 Xwayland 折成
 * v120/120 ≈ 0.008 格, DIX 永远凑不满 1 格增量, Button4/5 不产生
 * (r20261007-041520 探针实证: v120=-1 → emulate delta=-0.01
 * num_events=0)。注: v5-v7 的 axis_discrete 分支才是「格数」语义
 * (Xwayland 侧 120*discrete 折算), 不可混用。
 * 折算链: axis_value120(±120N) → dispatch_scroll_motion valuator
 * v120/120 = ±N → DIX emulate_scroll_button_events 按增量符号出
 * Button4/5 (increment=+1.0) → wine X11DRV_ButtonPress → SendInput
 * WHEEL。discrete>0 → Button5 (滚轮向下, winex11 → WM_MOUSEWHEEL 负
 * 值)。与 wayland 分支「向上=正=向下滚」同号。value 只喂平滑滚动路
 * 径 (winex11 不消费), 取 libinput 滚轮惯例 10/格。frame 必发 (D9)。 */
void display_input_inject_axis(int which, int steps)
{
    if (!g_seat)
        return;
    /* 游戏全屏适配 (2026-10-15): letterbox 黑边区滚轮吞掉 —— 与点击同
     * 口径 (黑边 = 死区); 节流日志防刷屏。 */
    if (g_ptr_in_letterbox)
    {
        static uint32_t said;
        if (said++ % 50 == 0)
            OHLOG("inject_axis SWALLOWED (letterbox) n=%{public}u", said);
        return;
    }
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
                                 (double)steps * 10.0, steps * 120,
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
    bool is_text;      /* Task 4: x11 路线 IME commit, text 复用 heap 串 */
    bool is_mm_ptr;    /* M4a-T5: x11 多窗按窗指针 (C1: napi 线程只入队,
                        * drain 在 loop 线程执行 —— wlr_seat/wl_display
                        * 全部事件循环线程专用) */
    bool is_mm_key;    /* 同上, 按窗键盘 */
    bool is_mm_axis;   /* M4d-review I1: 同上, 按窗轴 (滚轮), 复用
                        * axis_which/axis_steps + mm_* 字段 */
    bool is_mm_close;  /* 任务 #129 RF#3: 按窗关闭请求, 复用 mm_toplevel_id */
    bool is_mm_geom_size; /* 任务 #135: host→wine 几何回写 —— 尺寸半边
                           * (mm_lx/mm_ly = w/h) */
    bool is_mm_geom_move; /* 任务 #135: 位置半边 (mm_lx/mm_ly = x/y) */
    uint32_t keycode; /* is_button 时复用为 evdev 按钮码 */
    bool press;
    float nx, ny;
    int phase;
    int axis_which, axis_steps; /* is_axis: 0=纵向 1=横向, ±N 步 */
    char *text; /* is_clipboard/is_text: UTF-8 串 (heap) */
    /* is_mm_*: 按窗注入参数 (mm_lx/mm_ly 是 OHOS 承载窗局部物理像素,
     * 换算到 X 逻辑坐标在 loop 线程的 dispatch 里做 —— win_w 未校准
     * 前不知道比例) */
    uint32_t mm_toplevel_id;
    int mm_lx, mm_ly;
    int mm_action;
    uint32_t mm_button;
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
        else if (item.is_mm_ptr)
            x11_toplevel_input_pointer_dispatch(
                item.mm_toplevel_id, item.mm_lx, item.mm_ly,
                item.mm_action, item.mm_button);
        else if (item.is_mm_key)
            x11_toplevel_input_key_dispatch(
                item.mm_toplevel_id, item.keycode, item.press);
        else if (item.is_mm_axis)
            x11_toplevel_input_axis_dispatch(
                item.mm_toplevel_id, item.mm_lx, item.mm_ly,
                item.axis_which, item.axis_steps);
        else if (item.is_mm_close)
            x11_toplevel_close_dispatch(item.mm_toplevel_id);
        else if (item.is_mm_geom_size)
            x11_toplevel_geometry_size_dispatch(item.mm_toplevel_id,
                                                item.mm_lx, item.mm_ly);
        else if (item.is_mm_geom_move)
            x11_toplevel_geometry_move_dispatch(item.mm_toplevel_id,
                                                item.mm_lx, item.mm_ly);
        else if (item.is_axis)
            display_input_inject_axis(item.axis_which, item.axis_steps);
        else if (item.is_text)
        {
            /* Task 4: x11 路线 IME commit —— 转 xim bridge (NCP 子进程以
             * XIM_COMMIT 投给 wine)。与 is_clipboard 同所有权: 所有权随队列
             * 移交, 这里释放。桥未就绪 (chan 未建) 时 send_text 内部丢弃。 */
            if (xim_bridge_send_text(item.text) != 0)
                OHLOG("xim bridge send failed (chan down?), text dropped");
            free(item.text);
        }
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

uint32_t wl_ohos_input_last_ptr_window_id(void)
{
    return g_last_motion_win;
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

void wl_ohos_input_post_text(const char *utf8)
{
    if (!utf8 || !*utf8)
        return;
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_text = true;
    item.text = strdup(utf8);
    if (!item.text)
        return;
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

    /* #177: Xwayland 检测到该 global 才 get_relative_pointer 并把增量译成
     * XI2 raw motion (init_relative_pointer); 无绑定调用是 no-op, 创建失败
     * 只回退旧行为 (绝对坐标差), 不挡链 */
    g_rel_manager = wlr_relative_pointer_manager_v1_create(wl);
    if (!g_rel_manager)
        OH_LOG_ERROR(LOG_APP, "relative pointer manager create failed");

    /* 手动注入用的两个定时器 (队列键投递 + 脉冲释放)。真机门自动注入脚本
     * 已于 2026-10-13 退役 (职能被 smoke 编排 / 归档判定覆盖), 本处不再
     * 武装任何自走注入。 */
    if (g_loop)
    {
        g_key_timer = wl_event_loop_add_timer(g_loop, DeliverPendingKey,
                                              &g_pending_key);
        g_pulse_release_timer = wl_event_loop_add_timer(g_loop,
                                                        DeliverPulseRelease,
                                                        NULL);
    }
    OHLOG("seat up (keymap=%{public}s)", g_kb.keymap ? "ok" : "MISSING");
    return 0;
}

void wl_ohos_input_xwayland_set_seat(struct wlr_xwayland *xwayland)
{
    if (!g_seat || !xwayland)
        return;
    g_xwayland = xwayland; /* GAP2: SyncXInputFocus 用 (xwm 连接) */
    wlr_xwayland_set_seat(xwayland, g_seat);
    OHLOG("xwayland seat attached");
}

/* ── M4a-T5: x11 多窗模式按窗路由口 (x11_toplevel_input_* 的执行端) ──
 * 与 hit-test 路径 (post_motion) 的区别: 目标窗口由调用方给 (ArkTS 承载
 * 窗 → toplevelId → xs), 不走 client_topmost_at 命中; 坐标已是窗口局部
 * 值。纪律全部复用: FocusClient (activate+keyboard enter+settle 记录) /
 * pointer frame (D9) / button 脉冲 (D15/D30, inject_button 内部) / 注入
 * 队列 settle (键)。多窗模式无 g_clients/scene, raise 不适用 (z 序归
 * ArkTS 承载窗, inject_button 内的 client_raise 对非命中 xs 为 no-op)。 */
void wl_ohos_input_multimode_pointer(struct wlr_xwayland_surface *xs,
                                     double lx, double ly,
                                     int action, uint32_t button)
{
    if (!g_seat || !xs || !xs->surface)
        return;
    /* ArkTS MouseAction: Press=1 Release=2 Move=3 (input_manager.cpp 同注) */
    const int ACT_PRESS = 1;
    const int ACT_RELEASE = 2;
    const int ACT_MOVE = 3;

    if (xs->surface != g_ptr_focus)
    {
        FocusClient(xs); /* activate + keyboard enter + settle 记录 */
        if (g_display)
            wl_display_flush_clients(g_display); /* 焦点批先行 (同键纪律) */
        wlr_seat_pointer_notify_enter(g_seat, xs->surface, lx, ly);
        TrackPtrFocus(xs->surface);
    }
    /* g_ptr_focus_xs 供 pulse/断言读; 多窗模式不 raise (z 序归 ArkTS) */
    g_ptr_focus_xs = xs;

    if (action == ACT_MOVE)
    {
        wlr_seat_pointer_notify_motion(g_seat, NowMsec(), lx, ly);
        wlr_seat_pointer_notify_frame(g_seat); /* D9: motion 必配 frame */
        if (g_display)
            wl_display_flush_clients(g_display);
        return;
    }
    if (action == ACT_PRESS || action == ACT_RELEASE)
    {
        /* M4d (数据点①/② 实测): 按钮前必须过 motion —— 完整 wire 序列
         * (D15) 的 motion 半边在本口缺失。焦点 surface 未变时原实现直发
         * button: Xwayland 光标位置停在上一条 motion, 且 wine 菜单跟踪持
         * X grab 时 enter 被抑制 (X 侧无 EnterNotify, 光标不更新), 按钮带
         * 陈旧坐标经 grab 重定向落到菜单窗 → 命中判定全错: 真机 2026-10-11
         * (a) 外点 dismiss: 坐标恰在菜单 rect 内(锚点) → 菜单不关闭;
         * (b) 越界菜单项: 落点错位 → 关菜单无 WM_COMMAND (M4c §2.14 悬案
         * 的收敛面)。motion 补齐后按钮按真实落点投递 (grab 重定向下坐标
         * 同样为真)。motion 必配 frame (D9)。 */
        wlr_seat_pointer_notify_motion(g_seat, NowMsec(), lx, ly);
        wlr_seat_pointer_notify_frame(g_seat);
        display_input_inject_button(button, action == ACT_PRESS);
    }
}

void wl_ohos_input_multimode_key(struct wlr_xwayland_surface *xs,
                                 uint32_t keycode, bool press)
{
    if (!g_seat || !xs || !xs->surface)
        return;
    FocusClient(xs); /* 幂等 (同 surface 短路); 切窗首击也过 settle */
    wl_ohos_input_post_key(keycode, press);
}

/* M4d-review I1: 按窗轴注入 (x11 multimode 滚轮)。axis 事件不携带位置 ——
 * 与按钮同款处理 (8a1e975 的类别): 目标窗指针焦点未建立时先 enter (带
 * 坐标), 再无条件 motion+frame 把 X 光标带到滚动点, 然后才发 axis。X 侧
 * 投递链见 display_input_inject_axis 注释 (value120 → DIX → Button4/5 →
 * wine WM_MOUSEWHEEL, 按 X 光标落点路由): 缺 motion 半边时滚轮按上一条
 * motion 的陈旧位置投递 —— 与 M4d 数据点①/② 按钮陈旧坐标同一机理。 */
void wl_ohos_input_multimode_axis(struct wlr_xwayland_surface *xs,
                                  double lx, double ly,
                                  int which, int steps)
{
    if (!g_seat || !xs || !xs->surface)
        return;
    if (steps == 0)
        return;
    if (xs->surface != g_ptr_focus)
    {
        FocusClient(xs); /* activate + keyboard enter + settle 记录 */
        if (g_display)
            wl_display_flush_clients(g_display); /* 焦点批先行 (同键纪律) */
        wlr_seat_pointer_notify_enter(g_seat, xs->surface, lx, ly);
        TrackPtrFocus(xs->surface);
    }
    g_ptr_focus_xs = xs;
    wlr_seat_pointer_notify_motion(g_seat, NowMsec(), lx, ly);
    wlr_seat_pointer_notify_frame(g_seat); /* motion 必配 frame (D9) */
    display_input_inject_axis(which, steps);
}

/* M4c-T2: seat 焦点让渡导出口 (x11_toplevel.c 的 popup show/hide 调)。
 * 语义与 multimode 口的 FocusClient 完全一致 —— 这里只做导出包装, 不复制
 * 逻辑。调用方 (popup show/hide) 在合成器 loop 线程持映射层 g_lock 时调
 * (popup_visible 状态与焦点还原须同拍)。历史纪律是「FocusClient 内禁止
 * 回查映射层」; #167 起 FocusClient 内含模态自查 (x11_toplevel_modal_
 * blocks), 回入面用 trylock 消解 —— 持锁重入时 fail-open 放行, 结构上
 * 不可能死锁; popup 焦点让渡本就无需模态门, 放行即正确。 */
void wl_ohos_input_focus_xs(struct wlr_xwayland_surface *xs)
{
    FocusClient(xs);
}

/* ── M4a-T5 (C1 review 修复): 多窗注入的线程边界 ──
 * napi/JS 线程只入队 (本文件 InjectQueue 的加锁 + pipe 唤醒), loop 线程
 * 的 InjectQueueDrain 分派到 x11_toplevel_input_*_dispatch —— 后者在
 * loop 线程查 entry 表并执行 wlr_seat/XCB 操作。此前分叉在 JS 线程直调
 * wlr_seat/wl_display_flush (与 InputManager 的排队模型相悖, review C1)。 */
void wl_ohos_input_post_mm_pointer(uint32_t toplevelId, int lx, int ly,
                                   int action, uint32_t button)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_mm_ptr = true;
    item.mm_toplevel_id = toplevelId;
    item.mm_lx = lx;
    item.mm_ly = ly;
    item.mm_action = action;
    item.mm_button = button;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_mm_key(uint32_t toplevelId, uint32_t keycode,
                               bool press)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_mm_key = true;
    item.mm_toplevel_id = toplevelId;
    item.keycode = keycode;
    item.press = press;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_mm_axis(uint32_t toplevelId, int lx, int ly,
                                int which, int steps)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_mm_axis = true;
    item.mm_toplevel_id = toplevelId;
    item.mm_lx = lx;
    item.mm_ly = ly;
    item.axis_which = which;
    item.axis_steps = steps;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_mm_close(uint32_t toplevelId)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_mm_close = true;
    item.mm_toplevel_id = toplevelId;
    InjectQueuePush(&item);
}

/* 任务 #135: host→wine 几何回写两半 (napi 线程入队, loop 线程 dispatch ——
 * xcb 连接/entry 表纪律同 mm_close)。mm_lx/mm_ly 承载 (w,h) 或 (x,y)。 */
void wl_ohos_input_post_mm_geom_size(uint32_t toplevelId, int w, int h)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_mm_geom_size = true;
    item.mm_toplevel_id = toplevelId;
    item.mm_lx = w;
    item.mm_ly = h;
    InjectQueuePush(&item);
}

void wl_ohos_input_post_mm_geom_move(uint32_t toplevelId, int x, int y)
{
    struct inject_item item;
    memset(&item, 0, sizeof(item));
    item.is_mm_geom_move = true;
    item.mm_toplevel_id = toplevelId;
    item.mm_lx = x;
    item.mm_ly = y;
    InjectQueuePush(&item);
}
