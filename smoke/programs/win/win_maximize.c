/* winehua_t_win_maximize — 最大化/还原与 WM_GETMINMAXINFO（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 失败特征：min 不生效 = 可调整性判据/min-max 提取链断。
 */
#include "../common/winehua_t_check.h"

static int g_min_w = 400, g_min_h = 300;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_GETMINMAXINFO)
    {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = g_min_w;
        info->ptMinTrackSize.y = g_min_h;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    RECT work, r;
    int max_w, max_h;

    t_begin("winehua_t_win_maximize", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_WinMax";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    hwnd = CreateWindowExA(0, "WineHuaT_WinMax", "max", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 80, 600, 450, NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "hwnd=%p", hwnd);
    if (!hwnd)
        return t_finish();

    /* min 尺寸约束：尝试缩到 200x150，应被钳到 400x300 */
    TEXPR(SetWindowPos(hwnd, NULL, 0, 0, 200, 150, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    TEXPR(GetWindowRect(hwnd, &r));
    t_check("min-tracked", r.right - r.left >= g_min_w - 4 && r.bottom - r.top >= g_min_h - 4,
            "got %ldx%ld (min %dx%d)", (long)(r.right - r.left),
            (long)(r.bottom - r.top), g_min_w, g_min_h);

    /* 最大化：铺满工作区 */
    TEXPR(SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0));
    TEXPR(ShowWindow(hwnd, SW_MAXIMIZE));
    {
        int i;
        RECT r2;
        r2.left = r2.top = r2.right = r2.bottom = 0;
        /* 等待同步状态机落地（合成器应答链） */
        for (i = 0; i < 30 && !IsZoomed(hwnd); ++i)
            Sleep(50);
        TEXPR(IsZoomed(hwnd));
        TEXPR(GetWindowRect(hwnd, &r2));
        max_w = r2.right - r2.left;
        max_h = r2.bottom - r2.top;
        t_check("maximized-covers-workarea", max_w >= (r2.right - r2.left) &&
                max_w >= work.right - work.left - 8 && max_h >= work.bottom - work.top - 8,
                "window %dx%d workarea %ldx%ld", max_w, max_h,
                (long)(work.right - work.left), (long)(work.bottom - work.top));
        t_metric("maximized", "%dx%d", max_w, max_h);
    }

    TEXPR(ShowWindow(hwnd, SW_RESTORE));
    {
        int i;
        for (i = 0; i < 30 && IsZoomed(hwnd); ++i)
            Sleep(50);
        t_check("unzoomed", !IsZoomed(hwnd), "still zoomed");
        TEXPR(GetWindowRect(hwnd, &r));
        t_check("restore-size", r.right - r.left >= g_min_w && r.bottom - r.top >= g_min_h &&
                r.right - r.left < max_w, "restored %ldx%ld",
                (long)(r.right - r.left), (long)(r.bottom - r.top));
    }

    DestroyWindow(hwnd);
    return t_finish();
}
