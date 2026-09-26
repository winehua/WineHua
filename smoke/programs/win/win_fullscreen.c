/* winehua_t_win_fullscreen — 显示模式切换与全屏窗（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 失败特征：黑边/比例错 = letterbox 链；模式表缺 = 虚拟模式包络断。
 * 截帧 V 半段依赖宿主截帧通道，本例以本窗 DC 读回验证四象限（B 半段）。
 * 无论成败都恢复原显示模式，避免污染同会话后续用例。
 */
#include "../common/winehua_t_check.h"

static DWORD enum_mode_count(void)
{
    DEVMODEA dm;
    DWORD i = 0;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    while (EnumDisplaySettingsA(NULL, i, &dm))
    {
        ++i;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
    }
    return i;
}

/* 四象限颜色（B 半段：本窗 DC 读回） */
static void draw_quadrants(HDC dc, int w, int h)
{
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
    HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
    HBRUSH yellow = CreateSolidBrush(RGB(255, 255, 0));
    RECT r;

    r.left = 0; r.top = 0; r.right = w / 2; r.bottom = h / 2;
    FillRect(dc, &r, red);
    r.left = w / 2; r.right = w;
    FillRect(dc, &r, green);
    r.left = 0; r.right = w / 2; r.top = h / 2; r.bottom = h;
    FillRect(dc, &r, blue);
    r.left = w / 2; r.right = w;
    FillRect(dc, &r, yellow);
    DeleteObject(red);
    DeleteObject(green);
    DeleteObject(blue);
    DeleteObject(yellow);
}

int main(int argc, char **argv)
{
    DEVMODEA dm_orig, dm_new;
    DWORD modes;
    LONG rc;
    HWND wnd;
    RECT client;

    t_begin("winehua_t_win_fullscreen", argc, argv);

    memset(&dm_orig, 0, sizeof(dm_orig));
    dm_orig.dmSize = sizeof(dm_orig);
    t_check("enum-current", EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm_orig),
            "err=%lu", GetLastError());
    modes = enum_mode_count();
    t_metric("mode-table-size", "%lu", modes);
    t_check("mode-table-nonempty", modes >= 1, "modes=%lu", modes);

    /* 请求 1280x800 模拟模式 */
    memset(&dm_new, 0, sizeof(dm_new));
    dm_new.dmSize = sizeof(dm_new);
    dm_new.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    dm_new.dmPelsWidth = 1280;
    dm_new.dmPelsHeight = 800;
    rc = ChangeDisplaySettingsA(&dm_new, 0);
    t_metric("change-disp-rc", "%ld", (long)rc);
    t_check("change-mode-ok", rc == DISP_CHANGE_SUCCESSFUL, "rc=%ld", (long)rc);

    if (rc == DISP_CHANGE_SUCCESSFUL)
    {
        DEVMODEA dm_now;
        memset(&dm_now, 0, sizeof(dm_now));
        dm_now.dmSize = sizeof(dm_now);
        EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm_now);
        t_check("mode-applied", dm_now.dmPelsWidth == 1280 && dm_now.dmPelsHeight == 800,
                "now=%lux%lu", (unsigned long)dm_now.dmPelsWidth,
                (unsigned long)dm_now.dmPelsHeight);
    }

    /* 全屏窗：铺满当前模式 */
    wnd = t_create_toplevel("Static", "WineHuaT_FullScr", WS_POPUP | WS_VISIBLE, 0,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            "FS");
    t_check("create-fullscreen", wnd != NULL, "err=%lu", wnd ? 0 : GetLastError());
    if (wnd)
    {
        HDC dc;
        client.left = client.top = 0;
        client.right = client.bottom = 0;
        TEXPR(GetClientRect(wnd, &client));
        t_check("client-covers-mode",
                client.right >= GetSystemMetrics(SM_CXSCREEN) - 2 &&
                client.bottom >= GetSystemMetrics(SM_CYSCREEN) - 2,
                "client=%ldx%ld screen=%dx%d", (long)client.right, (long)client.bottom,
                GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        t_metric("fullscreen-client", "%ldx%ld", (long)client.right, (long)client.bottom);

        dc = GetDC(wnd);
        if (dc)
        {
            COLORREF c;
            draw_quadrants(dc, client.right, client.bottom);
            /* 四象限各取中心点读回（允许轻微容差） */
            c = GetPixel(dc, client.right / 4, client.bottom / 4);
            t_check("quad-red", GetRValue(c) > 200 && GetGValue(c) < 80 && GetBValue(c) < 80,
                    "got=%06lX", (unsigned long)c);
            c = GetPixel(dc, client.right * 3 / 4, client.bottom / 4);
            t_check("quad-green", GetGValue(c) > 200 && GetRValue(c) < 80 && GetBValue(c) < 80,
                    "got=%06lX", (unsigned long)c);
            c = GetPixel(dc, client.right / 4, client.bottom * 3 / 4);
            t_check("quad-blue", GetBValue(c) > 200 && GetRValue(c) < 80 && GetGValue(c) < 80,
                    "got=%06lX", (unsigned long)c);
            c = GetPixel(dc, client.right * 3 / 4, client.bottom * 3 / 4);
            t_check("quad-yellow", GetRValue(c) > 200 && GetGValue(c) > 200 && GetBValue(c) < 80,
                    "got=%06lX", (unsigned long)c);
            ReleaseDC(wnd, dc);
        }
        DestroyWindow(wnd);
    }

    /* 恢复原模式（成败都尝试，恢复失败也要暴露） */
    rc = ChangeDisplaySettingsA(&dm_orig, 0);
    t_check("restore-mode", rc == DISP_CHANGE_SUCCESSFUL, "rc=%ld", (long)rc);
    return t_finish();
}
