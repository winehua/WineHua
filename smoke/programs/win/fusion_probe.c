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
    HBRUSH br, bg, bb, bw, bk;

    GetClientRect(hwnd, &rc);
    br = CreateSolidBrush(RGB(255, 0, 0));
    bg = CreateSolidBrush(RGB(0, 255, 0));
    bb = CreateSolidBrush(RGB(0, 0, 255));
    bw = CreateSolidBrush(RGB(255, 255, 255));
    bk = CreateSolidBrush(RGB(0, 0, 0));

    /* 四象限 (rgba-quadrants-v1 同款排布): 左上 R / 右上 G / 左下 B / 右下 白 */
    q.left = rc.left; q.top = rc.top;
    q.right = (rc.left + rc.right) / 2; q.bottom = (rc.top + rc.bottom) / 2;
    FillRect(dc, &q, br);
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

    DeleteObject(br); DeleteObject(bg); DeleteObject(bb);
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
    char metrics[512];
    snprintf(metrics, sizeof(metrics),
             "{\"clickCounts\":{\"a\":%u,\"b\":%u},"
             "\"windowsCreated\":%s,\"fixedFrame\":\"fusion-two-window-v1\"}",
             state->clicks_a, state->clicks_b,
             state->windows_created ? "true" : "false");
    winehua_smoke_write_result(&state->options, status, stage, message, metrics);
}

int main(int argc, char **argv)
{
    struct probe_state state;
    MSG msg;
    ULONGLONG start_ms, deadline_ms, last_report;
    BOOL have_msg;

    memset(&state, 0, sizeof(state));
    if (!winehua_smoke_parse_options(&state.options, argc, argv, 12)) return 6;

    start_ms = GetTickCount64();
    deadline_ms = start_ms + state.options.seconds * 1000ULL;
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

    while ((have_msg = PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) != 0 || msg.message != WM_QUIT)
    {
        if (have_msg)
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            continue;
        }

        ULONGLONG now = GetTickCount64();
        if (now - last_report >= 2000)
        {
            last_report = now;
            report_heartbeat(&state, "RUNNING", "present", "heartbeat");
        }
        if (now >= deadline_ms)
            break;
        Sleep(15);
    }

    /* 终态: 双窗建出即 PASS (呈现/点击判定归 host 侧判定器 — 视觉证据与
     * 注入编排都在 host, guest 只负责事实上报)。 */
    {
        char message[128];
        BOOL passed = state.windows_created;
        snprintf(message, sizeof(message),
                 "clicks a=%u b=%u", state.clicks_a, state.clicks_b);
        report_heartbeat(&state, passed ? "PASS" : "FAIL", "present", message);
    }
    return state.windows_created ? 0 : 1;
}
