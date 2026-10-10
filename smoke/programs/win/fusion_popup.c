/*
 * fusion_popup — x11 fusion 弹出层 (OR 菜单) 自动化探针 (M4c-T4)。
 *
 * 单窗纯绿锚点 + TrackPopupMenu 程序化开关菜单，覆盖 M4c popup 链的
 * 自动判定域（渲染 / 关闭链 —— 已验证行为；点选不注入，T3 越界点选
 * 悬案移交 M4d，见 checks/fusion-popup-selection 的 SKIP 口径）：
 *
 *   缺省（fusion-popup 用例）: 菜单弹在窗界内 (140,160) —— notepad
 *   真实流同形态（菜单在窗 rect 内，T3 全绿域）。
 *   WINEHUA_POPUP_OOB=1（fusion-popup-oob 用例）: 短窗 + 菜单弹在客户
 *   区底缘下 —— 整条菜单越出窗 rect（T3 已证越界渲染；点选悬案未闭合）。
 *   负全局坐标（越出屏幕左/上）不在自动化范围 —— WMS 对负坐标行为未测
 *   （plan RF#5 后半），只留手动验证路径，不进套件。
 *
 * 链路: guest 写 inject-request（byTitle owner 点击，复刻 notepad 真实
 * 流的先导指针交互）→ SetForegroundWindow → TrackPopupMenu（模态阻塞；
 * 菜单循环对非鼠标/键盘消息走 NtUserDispatchMessage —— win32u/menu.c
 * 主循环 else 分支 —— 定时器照常进 wndproc）→ 首个 TIMER 抓菜单窗几何
 * （"#32768"）+ 宣告 fixed-frame（host 连抓 4 帧）→ 保持 10s 后
 * PostMessage WM_CANCELMODE（menu.c:4166 菜单循环见该消息即退出）→
 * 终态上报 popupSeq。
 *
 * 判定归 host: visual:fusion-popup-menu（菜单底色+条目结构；锚点窗纯绿
 * 不画白 —— 白块唯一来源 = 菜单）+ fusion-popup（开关链）。窗口用托管
 * 样式（popup_overflow 同款）—— WS_POPUP 会被 wine 判成不托管窗，窗与
 * 菜单全落 OR 路径不渲染（见 create_window 注释）；本栈非客户区不绘制，
 * 白块分类不受标题栏影响。
 *
 * Smoke protocol: --automation --run-id --test-id --result --seconds
 * （winehua_smoke_protocol.h 同款）。心跳常写 popupSeq，fixed-frame 在
 * 菜单在场时宣告一次。
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../../../thirdparty/wine/programs/winehua_smoke_protocol.h"

#define WIN_W 640
#define WIN_H 480
#define WIN_X 100
#define WIN_Y 100
/* oob 形态: 短窗（菜单弹底缘下整条越界），窗面积保锚点判定 ≥1% 屏 */
#define OOB_W 400
#define OOB_H 120
#define OOB_X 60
#define OOB_Y 50

#define TIMER_MENU 1
#define TIMER_PERIOD_MS 250
#define MENU_HOLD_MS 10000   /* 菜单保持: host 抓帧 4 张 (~2-6s) 后留富余 */
#define OWNER_WAIT_MS 6000   /* owner 注入点击等待上限（不到也继续开菜单）*/
#define MENU_TRACK_MIN_MS 3000 /* opened 事实门: 驻留低于此 = 菜单没弹出 */

static const char *kWndClass = "WineHuaFusionPopup";

struct popup_state
{
    struct winehua_smoke_options options;
    HWND wnd;
    BOOL oob;
    BOOL windows_created;
    BOOL owner_clicked;      /* 注入先导点击到达（只记录，不参与判定） */
    int phase;               /* 0 待开 / 1 菜单跟踪中 / 2 已关待终态 / 3 完成 */
    ULONGLONG track_enter_ms, track_first_ms, track_exit_ms;
    DWORD menu_result;
    BOOL menu_seen;          /* TIMER 在菜单窗在场时抓到 "#32768" */
    char menu_rect[64];      /* 菜单窗矩形（诊断数据；判定读帧不读它） */
    BOOL final_written;
};

static struct popup_state g_state;

/* 心跳/终态共用 metrics。popupSeq = fusion-popup 判定输入。 */
static void report_metrics(const char *status, const char *stage,
                           const char *message)
{
    char metrics[768];
    ULONGLONG track_ms = (g_state.track_exit_ms && g_state.track_enter_ms)
                             ? g_state.track_exit_ms - g_state.track_enter_ms
                             : 0;
    const char *expect = getenv("WINEHUA_SMOKE_EXPECT_ROUTE");
    const char *req = getenv("WINEHUA_DISPLAY_ROUTE");
    if (!expect || !expect[0]) expect = "-";
    if (!req || !req[0]) req = "-";
    snprintf(metrics, sizeof(metrics),
             "{\"popupSeq\":{\"ownerClicked\":%s,\"opened\":%s,\"closed\":%s,"
             "\"trackMs\":%llu,\"menuSeen\":%s,\"result\":%lu,"
             "\"menuRect\":\"%s\",\"holdTargetMs\":%d},"
             "\"windowsCreated\":%s,\"fixedFrame\":\"fusion-popup-menu-v1\","
             "\"expectedRoute\":\"%s\",\"requestedRoute\":\"%s\"}",
             g_state.owner_clicked ? "true" : "false",
             (track_ms >= MENU_TRACK_MIN_MS) ? "true" : "false",
             g_state.track_exit_ms ? "true" : "false",
             (unsigned long long)track_ms,
             g_state.menu_seen ? "true" : "false",
             (unsigned long)g_state.menu_result,
             g_state.menu_rect,
             MENU_HOLD_MS,
             g_state.windows_created ? "true" : "false",
             expect, req);
    winehua_smoke_write_result(&g_state.options, status, stage, message, metrics);
}

static LRESULT CALLBACK popup_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        HBRUSH green;
        GetClientRect(hwnd, &rc);
        green = CreateSolidBrush(RGB(0, 255, 0));
        FillRect(dc, &rc, green);
        DeleteObject(green);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* WM_PAINT 全幅覆盖 (防闪); 锚点窗不许出现非绿像素 */
    case WM_LBUTTONDOWN:
        g_state.owner_clicked = TRUE;
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_MENU && g_state.phase == 1)
        {
            ULONGLONG now = GetTickCount64();
            if (!g_state.track_first_ms)
            {
                HWND mwnd = FindWindowA("#32768", NULL);
                g_state.track_first_ms = now;
                if (mwnd)
                {
                    RECT r;
                    if (GetWindowRect(mwnd, &r))
                        snprintf(g_state.menu_rect, sizeof(g_state.menu_rect),
                                 "%ld,%ld,%ldx%ld", (long)r.left, (long)r.top,
                                 (long)(r.right - r.left),
                                 (long)(r.bottom - r.top));
                    g_state.menu_seen = TRUE;
                }
                /* fixed-frame 宣告: 菜单在场, host 轮询到即连抓 4 帧 */
                report_metrics("RUNNING", "fixed-frame", "fixed-frame");
            }
            else if (now - g_state.track_first_ms >= MENU_HOLD_MS)
            {
                /* 程序化关菜单: 菜单循环见 WM_CANCELMODE 即退出跟踪 */
                PostMessageA(hwnd, WM_CANCELMODE, 0, 0);
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static BOOL register_class(void)
{
    WNDCLASSA wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = popup_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = kWndClass;
    return RegisterClassA(&wc);
}

static BOOL create_window(void)
{
    int w = g_state.oob ? OOB_W : WIN_W;
    int h = g_state.oob ? OOB_H : WIN_H;
    int x = g_state.oob ? OOB_X : WIN_X;
    int y = g_state.oob ? OOB_Y : WIN_Y;
    /* 托管窗样式 (popup_overflow 同款, 跑 T3 真机形态): WS_POPUP 会被 wine
     * 判为不托管 → X 侧 override_redirect → 窗和菜单全走 OR-popup 路径
     * (owner 链上行找不到根 toplevel → OHOS 子窗不建, 真机 r20261011-035938
     * 实测窗/菜单全不渲染)。非客户区在本栈不渲染 (xwm 无 decor), 白块
     * 分类器不受标题栏干扰。 */
    DWORD style = (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX))
                  | WS_VISIBLE;

    g_state.wnd = CreateWindowExA(0, kWndClass, "fusion-P",
                                  style,
                                  x, y, w, h,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
    g_state.windows_created = g_state.wnd != NULL;
    return g_state.windows_created;
}

static void write_inject_request(void)
{
    char path[MAX_PATH];
    FILE *f;

    if (!g_state.options.automation || !g_state.options.test_id[0]) return;
    snprintf(path, sizeof(path), "C:\\smoke\\inject-request-%s.json",
             g_state.options.test_id);
    f = fopen(path, "w");
    if (!f) return;
    /* 两段式先导交互 (复刻 notepad 真实流): owner 窗先收到一次点击再开菜单 */
    fputs("{\"actions\":[{\"type\":\"sleep\",\"ms\":2000},"
          "{\"type\":\"byTitle\",\"title\":\"fusion-P\",\"button\":\"left\"}]}",
          f);
    fclose(f);
}

int main(int argc, char **argv)
{
    MSG msg;
    ULONGLONG now, deadline_ms, last_report, gate_ms, owner_deadline;
    HMENU menu;
    POINT origin;

    memset(&g_state, 0, sizeof(g_state));
    if (!winehua_smoke_parse_options(&g_state.options, argc, argv, 12)) return 6;
    {
        const char *oob = getenv("WINEHUA_POPUP_OOB");
        g_state.oob = oob && oob[0] == '1';
    }
    deadline_ms = GetTickCount64() + g_state.options.seconds * 1000ULL;
    /* 入口门相对本进程起点 (fusion_probe C1 教训: GetTickCount64 基点是
     * 设备启动, 裸 seconds/4 恒过 = 延迟死代码)。 */
    gate_ms = GetTickCount64() + g_state.options.seconds * 1000ULL / 4;
    owner_deadline = gate_ms + OWNER_WAIT_MS;
    last_report = 0;

    winehua_smoke_write_result(&g_state.options, "STARTED", "startup",
                               "fusion popup probe starting", "{}");
    if (!register_class())
    {
        winehua_smoke_write_result(&g_state.options, "FAIL", "startup",
                                   "RegisterClass failed", "{}");
        return 1;
    }
    if (!create_window())
    {
        winehua_smoke_write_result(&g_state.options, "FAIL", "startup",
                                   "CreateWindow failed", "{}");
        return 1;
    }
    write_inject_request();
    SetTimer(g_state.wnd, TIMER_MENU, TIMER_PERIOD_MS, NULL);

    for (;;)
    {
        now = GetTickCount64();
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        now = GetTickCount64();

        if (g_state.phase == 0 && now >= gate_ms &&
            (g_state.owner_clicked || now >= owner_deadline))
        {
            /* 先导交互 (两段式): owner 点击到了再开菜单; 注入编排不是
             * popup 链判据, 等待超限也继续。 */
            SetForegroundWindow(g_state.wnd);
            menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING, 3001, "Alpha");
            AppendMenuA(menu, MF_STRING, 3002, "Beta");
            AppendMenuA(menu, MF_STRING, 3003, "Gamma");
            if (g_state.oob)
            {
                /* 客户区左下角 (屏幕坐标): 菜单自窗底缘向下整条越界 */
                origin.x = 0;
                origin.y = OOB_H;
                ClientToScreen(g_state.wnd, &origin);
            }
            else
            {
                /* 窗界内 (notepad 形态): 菜单整体落在窗 rect 内 */
                origin.x = WIN_X + 40;
                origin.y = WIN_Y + 60;
            }
            g_state.phase = 1;
            g_state.track_enter_ms = GetTickCount64();
            g_state.menu_result = TrackPopupMenu(
                menu, TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD,
                origin.x, origin.y, 0, g_state.wnd, NULL);
            g_state.track_exit_ms = GetTickCount64();
            DestroyMenu(menu);
            g_state.phase = 2;
        }
        if (g_state.phase == 2 && !g_state.final_written)
        {
            ULONGLONG track_ms = g_state.track_exit_ms - g_state.track_enter_ms;
            char message[192];
            BOOL passed = g_state.windows_created &&
                          (g_state.menu_seen || track_ms >= MENU_TRACK_MIN_MS);
            g_state.final_written = TRUE;
            snprintf(message, sizeof(message),
                     "oob=%d track=%llums result=%lu seen=%d rect=%s",
                     g_state.oob ? 1 : 0,
                     (unsigned long long)track_ms,
                     (unsigned long)g_state.menu_result,
                     g_state.menu_seen ? 1 : 0,
                     g_state.menu_rect);
            report_metrics(passed ? "PASS" : "FAIL", "present", message);
            g_state.phase = 3;
        }

        if (!g_state.final_written && now - last_report >= 2000)
        {
            last_report = now;
            report_metrics("RUNNING", "present", "heartbeat");
        }
        if (now >= deadline_ms)
            break;
        Sleep(15);
    }

    KillTimer(g_state.wnd, TIMER_MENU);
    if (!g_state.final_written)
        report_metrics("FAIL", "timeout", "deadline before menu sequence done");
    if (g_state.wnd) DestroyWindow(g_state.wnd);
    return g_state.final_written ? 0 : 1;
}
