/*
 * WineHua fusion probe (M4a-T6): 双窗 GDI 探针 —— x11 fusion 多窗模式的
 * 呈现 + 按窗输入端到端判定源。
 *
 * 两个并排不重叠的顶层窗 (guest 逻辑坐标):
 *   A @ (100,100)  640x480  纯红 + 中心白十字  (fusion-window-a 判定)
 *   B @ (760,100)  640x480  四象限 (左上 R / 右上 G / 左下 B / 右下 白,
 *                           各 2px 黑边)       (fusion-window-b 判定)
 * WM_LBUTTONDOWN 按窗计数 → result JSON clickCounts {"a":N,"b":M}
 * (wine-trace:fusion-click-a 判定: 点击窗 A 后 a>=1 且 b==0)。
 *
 * GDI 自画 (不依赖 GL 私有通道): 多窗模式 guest GL 帧无消费者 (D50 附带
 * 缺口, M4b), 本探针的呈现走 winex11 shm → wl_surface commit → per-xs
 * render pass, 与 GDI 应用同路径。
 *
 * Smoke protocol: --automation --run-id --test-id --result --seconds
 * (wine submodule 的 winehua_smoke_protocol.h, 与其它探针同款)。消息循环
 * 常驻, 2s 心跳写 RUNNING (clickCounts 实时), deadline 写终态退出。
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../../../thirdparty/wine/programs/winehua_smoke_protocol.h"

#define WIN_W 640
#define WIN_H 480
#define WIN_A_X 100
#define WIN_A_Y 100
#define WIN_B_X 760
#define WIN_B_Y 100

static const char *kWndClassA = "WineHuaFusionA";
static const char *kWndClassB = "WineHuaFusionB";

struct probe_state
{
    struct winehua_smoke_options options;
    HWND wnd_a;
    HWND wnd_b;
    unsigned clicks_a;
    unsigned clicks_b;
    BOOL windows_created;
    /* M4b-T3: 状态序列 (--probe-state env 开关)。状态确认驱动 (RF#5):
     * 每步动作后轮询 win32 状态 ≤2s, 确认才记 stateSeq, 不用固定 sleep。 */
    BOOL probe_state;        /* env WINEHUA_SMOKE_PROBE_STATE=1 */
    int state_step;          /* 0..N 步骤游标; <0 = 序列结束 */
    ULONGLONG step_since_ms; /* 当前步骤动作发出的时刻 (超时判失败) */
    RECT a_orig_rect;        /* 全屏步还原基准 */
    LONG a_orig_style;
    BOOL seq_minimized, seq_restored, seq_fullscreen, seq_unfullscreen, seq_modal;
    char seq_note[128];      /* 首个失败步骤的原因 (判定器提示用) */
    ULONGLONG start_ms;      /* main 起点 tick。入口延迟必须相对它: 本构建
                              * GetTickCount64 = CLOCK_BOOTTIME (设备启动起算,
                              * wineserver monotonic_counter, wine
                              * server/request.c:508-520), 裸 seconds/4 恒过
                              * = 延迟死代码 (M4b final review C1)。 */
};

/* 中心白十字: 半长 1/4 窗宽/高, 臂宽 12px (逻辑坐标, 缩放后仍可辨) */
static void paint_cross(HDC dc, RECT *rc)
{
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    int cx = (rc->left + rc->right) / 2;
    int cy = (rc->top + rc->bottom) / 2;
    int arm_w = 12;
    int arm_h = (rc->bottom - rc->top) / 4;
    int arm_w2 = (rc->right - rc->left) / 4;
    RECT bar;

    /* 横臂 */
    bar.left = cx - arm_w2; bar.right = cx + arm_w2;
    bar.top = cy - arm_w / 2; bar.bottom = cy + arm_w / 2;
    FillRect(dc, &bar, white);
    /* 竖臂 */
    bar.left = cx - arm_w / 2; bar.right = cx + arm_w / 2;
    bar.top = cy - arm_h; bar.bottom = cy + arm_h;
    FillRect(dc, &bar, white);
    DeleteObject(white);
}

static void paint_window_a(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));

    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, red);
    DeleteObject(red);
    paint_cross(dc, &rc);
    EndPaint(hwnd, &ps);
}

static void paint_window_b(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, q;
    HBRUSH bm, bg, bb, bw, bk;

    GetClientRect(hwnd, &rc);
    /* I2 (review): 左上用品红 (255,0,255) 不用纯红 —— 窗 A 是纯红主体,
     * B 的左上若也用红, 全屏分类器会把两窗的红色并成一个 bbox, 拓扑
     * 判定结构性误判。品红保留「四色象限拓扑」语义且与 A 可分离。
     * 终验预演 (2026-10-10): 白色同病 —— 窗 A 的白十字与 B 的白象限
     * 跨窗并 bbox, 拓扑同样结构误判; 右下改青 (0,255,255), 与
     * frame.py 分类器成对改 (原则 25)。 */
    bm = CreateSolidBrush(RGB(255, 0, 255));
    bg = CreateSolidBrush(RGB(0, 255, 0));
    bb = CreateSolidBrush(RGB(0, 0, 255));
    bw = CreateSolidBrush(RGB(0, 255, 255));
    bk = CreateSolidBrush(RGB(0, 0, 0));

    /* 四象限: 左上 M / 右上 G / 左下 B / 右下 白 */
    q.left = rc.left; q.top = rc.top;
    q.right = (rc.left + rc.right) / 2; q.bottom = (rc.top + rc.bottom) / 2;
    FillRect(dc, &q, bm);
    q.left = (rc.left + rc.right) / 2; q.right = rc.right;
    FillRect(dc, &q, bg);
    q.left = rc.left; q.right = (rc.left + rc.right) / 2;
    q.top = (rc.top + rc.bottom) / 2; q.bottom = rc.bottom;
    FillRect(dc, &q, bb);
    q.left = (rc.left + rc.right) / 2; q.right = rc.right;
    FillRect(dc, &q, bw);

    /* 2px 黑十字边 (象限分隔线) */
    q = rc; q.right = q.left + 2; FillRect(dc, &q, bk);
    q = rc; q.bottom = q.top + 2; FillRect(dc, &q, bk);
    q = rc; q.left = q.right - 2; FillRect(dc, &q, bk);
    q = rc; q.top = q.bottom - 2; FillRect(dc, &q, bk);
    q.left = (rc.left + rc.right) / 2 - 1; q.right = q.left + 2; q.top = rc.top; q.bottom = rc.bottom;
    FillRect(dc, &q, bk);
    q.top = (rc.top + rc.bottom) / 2 - 1; q.bottom = q.top + 2; q.left = rc.left; q.right = rc.right;
    FillRect(dc, &q, bk);

    DeleteObject(bm); DeleteObject(bg); DeleteObject(bb);
    DeleteObject(bw); DeleteObject(bk);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK probe_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct probe_state *state;

    if (msg == WM_NCCREATE)
    {
        state = (struct probe_state *)((LPCREATESTRUCTA)lparam)->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    state = (struct probe_state *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    if (!state) return DefWindowProcA(hwnd, msg, wparam, lparam);

    switch (msg)
    {
    case WM_PAINT:
        if (hwnd == state->wnd_a) paint_window_a(hwnd);
        else if (hwnd == state->wnd_b) paint_window_b(hwnd);
        return 0;
    case WM_LBUTTONDOWN:
        if (hwnd == state->wnd_a) ++state->clicks_a;
        else if (hwnd == state->wnd_b) ++state->clicks_b;
        return 0;
    case WM_ERASEBKGND:
        return 1; /* WM_PAINT 全幅覆盖, 无需擦除 (防闪) */
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static BOOL register_classes(void)
{
    WNDCLASSA wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = probe_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);

    wc.lpszClassName = kWndClassA;
    if (!RegisterClassA(&wc)) return FALSE;
    wc.lpszClassName = kWndClassB;
    return RegisterClassA(&wc);
}

static BOOL create_windows(struct probe_state *state)
{
    state->wnd_a = CreateWindowExA(0, kWndClassA, "fusion-A",
                                   WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                   WIN_A_X, WIN_A_Y, WIN_W, WIN_H,
                                   NULL, NULL, GetModuleHandleA(NULL), state);
    state->wnd_b = CreateWindowExA(0, kWndClassB, "fusion-B",
                                   WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                   WIN_B_X, WIN_B_Y, WIN_W, WIN_H,
                                   NULL, NULL, GetModuleHandleA(NULL), state);
    state->windows_created = state->wnd_a && state->wnd_b;
    return state->windows_created;
}

static void report_heartbeat(struct probe_state *state, const char *status,
                             const char *stage, const char *message)
{
    char metrics[768];
    /* 路线自报 (如实环境事实, 非呈现归因): GDI/shm 呈现链没有 graphics
     * smoke 的 surfaceKey/displayStallMs 通道, presented-route 三段判定
     * 不适用 (checks 已移除, 见 test.json); expectedRoute 存在性由
     * fusion-clicks 判定器校验 —— env 被丢 = 冷启参数被丢 = 本判据抓点。 */
    const char *expect = getenv("WINEHUA_SMOKE_EXPECT_ROUTE");
    const char *req = getenv("WINEHUA_DISPLAY_ROUTE");
    if (!expect || !expect[0]) expect = "-";
    if (!req || !req[0]) req = "-";
    if (state->probe_state)
        snprintf(metrics, sizeof(metrics),
                 "{\"clickCounts\":{\"a\":%u,\"b\":%u},"
                 "\"windowsCreated\":%s,\"fixedFrame\":\"fusion-two-window-v1\","
                 "\"expectedRoute\":\"%s\",\"requestedRoute\":\"%s\","
                 "\"stateSeq\":{\"minimized_a\":%s,\"restored_a\":%s,"
                 "\"fullscreen_a\":%s,\"unfullscreen_a\":%s,"
                 "\"modal_b_owned_a\":%s},\"stateNote\":\"%s\"}",
                 state->clicks_a, state->clicks_b,
                 state->windows_created ? "true" : "false",
                 expect, req,
                 state->seq_minimized ? "true" : "false",
                 state->seq_restored ? "true" : "false",
                 state->seq_fullscreen ? "true" : "false",
                 state->seq_unfullscreen ? "true" : "false",
                 state->seq_modal ? "true" : "false",
                 state->seq_note);
    else
        snprintf(metrics, sizeof(metrics),
                 "{\"clickCounts\":{\"a\":%u,\"b\":%u},"
                 "\"windowsCreated\":%s,\"fixedFrame\":\"fusion-two-window-v1\","
                 "\"expectedRoute\":\"%s\",\"requestedRoute\":\"%s\"}",
                 state->clicks_a, state->clicks_b,
                 state->windows_created ? "true" : "false",
                 expect, req);
    winehua_smoke_write_result(&state->options, status, stage, message, metrics);
}

/* ── M4b-T3: 状态序列状态机 (空闲时驱动, 每轮最多一步动作) ──
 * 步骤: 动作 → 后续轮询 win32 状态确认 (≤2s) → 记 stateSeq。确认驱动
 * (RF#5): wine→x11drv→xwm→host 链有延迟, 按 sleep 写终态会把「未达」
 * 写成「已达」。失败 = 记 seq_note 后跳过余下步骤 (判定器按缺步 FAIL)。 */

#define STEP_MINIMIZE 0
#define STEP_RESTORE 1
#define STEP_FULLSCREEN 2
#define STEP_UNFULLSCREEN 3
#define STEP_MODAL 4
#define STEP_DONE 5
#define STEP_CONFIRM_MS 2000

static void state_seq_begin(struct probe_state *state)
{
    state->probe_state = TRUE;
    state->state_step = -1; /* -1 = 等双窗稳定后从 STEP_MINIMIZE 进入 */
}

/* 当前步骤的确认条件; 达成返回 TRUE */
static BOOL state_seq_confirmed(struct probe_state *state)
{
    switch (state->state_step)
    {
    case STEP_MINIMIZE: return IsIconic(state->wnd_a) ? TRUE : FALSE;
    case STEP_RESTORE: return !IsIconic(state->wnd_a) ? TRUE : FALSE;
    case STEP_FULLSCREEN:
    {
        RECT rc, dr;
        if (!GetWindowRect(state->wnd_a, &rc)) return FALSE;
        dr.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        dr.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        dr.right = dr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        dr.bottom = dr.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return rc.left <= dr.left && rc.top <= dr.top &&
               rc.right >= dr.right && rc.bottom >= dr.bottom ? TRUE : FALSE;
    }
    case STEP_UNFULLSCREEN:
    {
        RECT rc;
        if (!GetWindowRect(state->wnd_a, &rc)) return FALSE;
        return rc.left == state->a_orig_rect.left &&
               rc.top == state->a_orig_rect.top &&
               (rc.right - rc.left) == (state->a_orig_rect.right -
                                        state->a_orig_rect.left) &&
               (rc.bottom - rc.top) == (state->a_orig_rect.bottom -
                                        state->a_orig_rect.top) ? TRUE : FALSE;
    }
    case STEP_MODAL:
        return (LONG_PTR)GetWindowLongPtrA(state->wnd_b, GWLP_HWNDPARENT) ==
               (LONG_PTR)state->wnd_a ? TRUE : FALSE;
    default: return FALSE;
    }
}

/* 发出当前步骤的动作 (每步只发一次, 由 step_since_ms==0 门控) */
static void state_seq_act(struct probe_state *state)
{
    switch (state->state_step)
    {
    case STEP_MINIMIZE:
        ShowWindow(state->wnd_a, SW_MINIMIZE);
        break;
    case STEP_RESTORE:
        ShowWindow(state->wnd_a, SW_RESTORE);
        break;
    case STEP_FULLSCREEN:
        GetWindowRect(state->wnd_a, &state->a_orig_rect);
        state->a_orig_style = GetWindowLongA(state->wnd_a, GWL_STYLE);
        /* 去边框 + 全屏矩形: x11drv 据此置 _NET_WM_STATE_FULLSCREEN →
         * xwm request_fullscreen → host Fullscreen 事件链 */
        SetWindowLongA(state->wnd_a, GWL_STYLE,
                       (state->a_orig_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(state->wnd_a, HWND_TOP,
                     GetSystemMetrics(SM_XVIRTUALSCREEN),
                     GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN),
                     GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     SWP_FRAMECHANGED);
        break;
    case STEP_UNFULLSCREEN:
        SetWindowLongA(state->wnd_a, GWL_STYLE, state->a_orig_style);
        SetWindowPos(state->wnd_a, HWND_TOP,
                     state->a_orig_rect.left, state->a_orig_rect.top,
                     state->a_orig_rect.right - state->a_orig_rect.left,
                     state->a_orig_rect.bottom - state->a_orig_rect.top,
                     SWP_FRAMECHANGED);
        break;
    case STEP_MODAL:
        /* owned 关系: wnd_b 的 owner = wnd_a (GWLP_HWNDPARENT)。host 侧
         * xwm set_parent → Modal 事件 → ModalWindowManager 跟随链 */
        SetWindowLongPtrA(state->wnd_b, GWLP_HWNDPARENT,
                          (LONG_PTR)state->wnd_a);
        break;
    default:
        break;
    }
}

/* 空闲时驱动 (PeekMessage 排空后调用)。返回 TRUE = 序列已结束。 */
static BOOL state_seq_tick(struct probe_state *state)
{
    ULONGLONG now;
    if (state->state_step == STEP_DONE) return TRUE;

    now = GetTickCount64();
    if (state->state_step < 0)
    {
        /* 入口延迟 (run 相对): 双窗 attach+首帧稳定 (宿主侧 ~2s), 序列总
         * 预算 12s 内。必须与 start_ms + seconds/4 比较 —— GetTickCount64
         * 基点是设备启动 (见 struct start_ms 注释), 裸 seconds/4 恒过,
         * 序列会在首个空闲 tick (~15ms) 触发, 宿主 attach+首帧的等待门
         * 形同虚设 (final review C1)。 */
        if (now < state->start_ms +
                  (ULONGLONG)state->options.seconds * 1000ULL / 4)
            return FALSE;
        state->state_step = STEP_MINIMIZE;
        state->step_since_ms = 0;
    }

    if (state->step_since_ms == 0)
    {
        state_seq_act(state);
        state->step_since_ms = now ? now : 1;
        return FALSE;
    }

    if (state_seq_confirmed(state))
    {
        switch (state->state_step)
        {
        case STEP_MINIMIZE: state->seq_minimized = TRUE; break;
        case STEP_RESTORE: state->seq_restored = TRUE; break;
        case STEP_FULLSCREEN: state->seq_fullscreen = TRUE; break;
        case STEP_UNFULLSCREEN: state->seq_unfullscreen = TRUE; break;
        case STEP_MODAL: state->seq_modal = TRUE; break;
        default: break;
        }
        state->state_step++;
        state->step_since_ms = 0;
        if (state->state_step == STEP_DONE)
            return TRUE;
        return FALSE;
    }

    if (now - state->step_since_ms > STEP_CONFIRM_MS)
    {
        /* 确认超时: 记失败原因, 跳过余下 (判定器按缺步 FAIL) */
        const char *names[] = { "minimized_a", "restored_a", "fullscreen_a",
                                "unfullscreen_a", "modal_b_owned_a" };
        if (state->state_step >= STEP_MINIMIZE &&
            state->state_step <= STEP_MODAL)
            snprintf(state->seq_note, sizeof(state->seq_note),
                     "timeout at %s", names[state->state_step]);
        state->state_step = STEP_DONE;
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    struct probe_state state;
    MSG msg;
    ULONGLONG start_ms, deadline_ms, last_report;

    memset(&state, 0, sizeof(state));
    if (!winehua_smoke_parse_options(&state.options, argc, argv, 12)) return 6;

    start_ms = GetTickCount64();
    deadline_ms = start_ms + state.options.seconds * 1000ULL;
    state.start_ms = start_ms;
    last_report = 0;

    winehua_smoke_write_result(&state.options, "STARTED", "startup",
                               "fusion probe starting", "{}");
    if (!register_classes())
    {
        winehua_smoke_write_result(&state.options, "FAIL", "startup",
                                   "RegisterClass failed", "{}");
        return 1;
    }
    if (!create_windows(&state))
    {
        winehua_smoke_write_result(&state.options, "FAIL", "startup",
                                   "CreateWindow failed", "{}");
        return 1;
    }

    /* I4: 声明按窗点击 (byTitle fusion-A) —— SmokeRunner 轮询发现后执行
     * ( ArkTS 侧按 title 找承载窗, 点窗内中心, 走按窗路由)。仅自动化模式
     * 写; 文件协议与 D28 的 inject-request-<testId>.json 同款。 */
    if (state.options.automation && state.options.test_id[0])
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "C:\\smoke\\inject-request-%s.json",
                 state.options.test_id);
        FILE *f = fopen(path, "w");
        if (f)
        {
            fputs("{\"actions\":[{\"type\":\"sleep\",\"ms\":2500},"
                  "{\"type\":\"byTitle\",\"title\":\"fusion-A\","
                  "\"button\":\"left\"}]}", f);
            fclose(f);
        }
    }

    /* M9 (review): 不依赖 msg 残值判断退出; 心跳/deadline 在队列排空后
     * 每轮都检查 (不被消息流饿死)。 */
    BOOL fixed_frame_announced = FALSE;
    /* M4b-T3: env 开关 (--probe-state 等价物, 不动 smoke 协议头) */
    {
        const char *ps = getenv("WINEHUA_SMOKE_PROBE_STATE");
        if (ps && ps[0] == '1') state_seq_begin(&state);
    }
    for (;;)
    {
        BOOL got = PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE);
        if (!got)
        {
            ULONGLONG now = GetTickCount64();
            if (state.probe_state)
                state_seq_tick(&state);
            if (now - last_report >= 2000)
            {
                last_report = now;
                report_heartbeat(&state, "RUNNING", "present", "heartbeat");
            }
            /* 帧采集协议: host 的 poll_run 只在 result 出现 "fixed-frame"
             * (stage+message) 字面量时截屏 —— 进入末 2 秒宣告一次
             * (graphics_smoke main.c:883 同款)。首跑 (job-r20261010-170854)
             * 缺此宣告 = missing-frame 根因。 */
            if (!fixed_frame_announced && now >= deadline_ms - 2000)
            {
                fixed_frame_announced = TRUE;
                report_heartbeat(&state, "RUNNING", "fixed-frame", "fixed-frame");
            }
            if (now >= deadline_ms)
                break;
            Sleep(15);
            continue;
        }
        if (msg.message == WM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* 终态: 双窗建出即 PASS (呈现/点击判定归 host 侧判定器 — 视觉证据与
     * 注入编排都在 host, guest 只负责事实上报)。 */
    {
        char message[192];
        BOOL passed = state.windows_created;
        if (state.probe_state)
            snprintf(message, sizeof(message),
                     "clicks a=%u b=%u seq m%d r%d f%d u%d o%d %s",
                     state.clicks_a, state.clicks_b,
                     state.seq_minimized, state.seq_restored,
                     state.seq_fullscreen, state.seq_unfullscreen,
                     state.seq_modal,
                     state.seq_note[0] ? state.seq_note : "ok");
        else
            snprintf(message, sizeof(message),
                     "clicks a=%u b=%u", state.clicks_a, state.clicks_b);
        report_heartbeat(&state, passed ? "PASS" : "FAIL", "present", message);
    }
    return state.windows_created ? 0 : 1;
}
