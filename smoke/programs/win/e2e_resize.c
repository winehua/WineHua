/* winehua_t_e2e_resize — 尺寸链（P1，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.15。
 * 失败特征：尺寸错 = resize configure 链断（win32u/合成器 resize 回归哨兵）。
 * 五组尺寸：每次 SetWindowPos 后断言 WM_SIZE 到达、客户区 == 窗口尺寸−边框
 * （AdjustWindowRect 换算）、wParam == SIZE_RESTORED。
 */
#include "../common/winehua_t_check.h"

static const int g_sizes[][2] = {
    {400, 300}, {500, 350}, {600, 400}, {300, 200}, {420, 320},
};
#define N_SIZES (sizeof(g_sizes) / sizeof(g_sizes[0]))

static volatile LONG g_size_seen;
static volatile LONG g_last_wparam;
static volatile LONG g_size_w, g_size_h;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_SIZE)
    {
        g_last_wparam = (LONG)wparam;
        g_size_w = (LONG)LOWORD(lparam);
        g_size_h = (LONG)HIWORD(lparam);
        InterlockedExchange(&g_size_seen, g_size_seen + 1);
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    int i, border_x, border_y;

    t_begin("winehua_t_e2e_resize", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_E2EResize";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    hwnd = CreateWindowExA(0, "WineHuaT_E2EResize", "resize",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           50, 40, 260, 180, /* 与首轮目标不同 → 必发 WM_SIZE */
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    {
        MSG msg;
        for (i = 0; i < 10; ++i)
        {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            Sleep(30);
        }
    }

    /* 边框度量以运行时实测为准（AdjustWindowRect 与 DPI 实测有 ~2px 舍入差） */
    {
        RECT wr, cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        border_x = (int)((wr.right - wr.left) - (cr.right - cr.left));
        border_y = (int)((wr.bottom - wr.top) - (cr.bottom - cr.top));
        t_metric("border", "%dx%d", border_x, border_y);
    }

    for (i = 0; i < (int)N_SIZES; ++i)
    {
        char name[40];
        int client_w, client_h;
        LONG before;

        /* 期望客户区 = 窗口尺寸 − 实测边框 */
        client_w = g_sizes[i][0] - border_x;
        client_h = g_sizes[i][1] - border_y;

        InterlockedExchange(&g_size_seen, 0);
        TEXPR(SetWindowPos(hwnd, NULL, 0, 0, g_sizes[i][0], g_sizes[i][1],
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        /* 等配置链落地（合成器 configure 往返） */
        {
            MSG msg;
            DWORD until = GetTickCount() + 2000;
            while (GetTickCount() < until && !g_size_seen)
            {
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                Sleep(20);
            }
        }
        before = g_size_seen;

        snprintf(name, sizeof(name), "size-%d-wm", i);
        t_check(name, before > 0, "no WM_SIZE");
        snprintf(name, sizeof(name), "size-%d-client", i);
        t_check(name, g_size_w == client_w && g_size_h == client_h,
                "WM_SIZE %ldx%ld expect %dx%d", (long)g_size_w, (long)g_size_h,
                client_w, client_h);
        snprintf(name, sizeof(name), "size-%d-wparam", i);
        t_check(name, g_last_wparam == SIZE_RESTORED, "wparam=%ld", (long)g_last_wparam);

        /* GetClientRect 与 WM_SIZE 一致 */
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            snprintf(name, sizeof(name), "size-%d-rect", i);
            t_check(name, rc.right - rc.left == client_w && rc.bottom - rc.top == client_h,
                    "rect %ldx%ld expect %dx%d", (long)(rc.right - rc.left),
                    (long)(rc.bottom - rc.top), client_w, client_h);
        }
    }

    t_metric("resize-rounds", "%d", (int)N_SIZES);
    DestroyWindow(hwnd);
    return t_finish();
}
