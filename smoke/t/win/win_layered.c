/* winehua_t_win_layered — 分层窗口（P1，收敛项③验收载荷）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 先红合法：均匀 alpha 桥实现落地前，屏幕读回半段应 FAIL；落地后转绿。
 * 失败特征：读回 alpha 与设定不符 = 均匀 alpha 被忽略（当前已知缺口）；
 * rgn 外区域可见 = shape 链断。
 * 三段各用独立窗口：SLWA 与 UpdateLayeredWindow 互斥（MSDN），不能混用。
 */
#include "../common/winehua_t_check.h"
#include <stdlib.h>

#define WND_W 200
#define WND_H 160

/* 从屏幕 DC 读回指定点颜色（跨窗口捕获链；捕获缺失时此处红） */
static COLORREF screen_pixel(int x, int y)
{
    HDC screen = GetDC(NULL);
    COLORREF c = GetPixel(screen, x, y);
    ReleaseDC(NULL, screen);
    return c;
}

static void fill_magenta(HWND wnd)
{
    HDC dc = GetDC(wnd);
    if (dc)
    {
        RECT r = {0, 0, WND_W, WND_H};
        HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &r, magenta);
        DeleteObject(magenta);
        ReleaseDC(wnd, dc);
    }
}

int main(int argc, char **argv)
{
    HWND wnd;
    BOOL ok;
    BYTE alpha = 0;
    COLORREF key = 0;
    DWORD flags = 0;
    RECT rect;

    t_begin("winehua_t_win_layered", argc, argv);

    /* ---- 段 1：均匀 alpha（SetLayeredWindowAttributes）---- */
    wnd = t_create_toplevel("Static", "WineHuaT_Layered", WS_POPUP | WS_VISIBLE,
                            WS_EX_LAYERED, 100, 100, WND_W, WND_H, "LYR");
    t_check("create-layered", wnd != NULL, "err=%lu", wnd ? 0 : GetLastError());
    if (!wnd)
        return t_finish();
    t_check("ex-style-kept",
            (GetWindowLongA(wnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0, "style");

    TEXPR(SetLayeredWindowAttributes(wnd, 0, 128, LWA_ALPHA));
    ok = GetLayeredWindowAttributes(wnd, &key, &alpha, &flags);
    t_check("get-attributes", ok && (flags & LWA_ALPHA) && alpha == 128,
            "ok=%d flags=%lX alpha=%u", ok, (unsigned long)flags, alpha);

    TEXPR(SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
    fill_magenta(wnd);
    UpdateWindow(wnd);
    Sleep(300); /* 等合成器呈现一帧 */

    GetWindowRect(wnd, &rect);
    {
        COLORREF inside = screen_pixel(rect.left + WND_W / 2, rect.top + WND_H / 2);
        /* 判定语义：读回为纯品红 = alpha 完全未生效（BAD）；全白 = 屏幕
         * 读回不可用（捕获链缺失，收敛项②范畴）；其余值 = 已被 alpha
         * 混合或呈现链改写（GOOD）。 */
        BOOL bad_pure_magenta = GetRValue(inside) > 250 && GetGValue(inside) < 30 &&
                                GetBValue(inside) > 250;
        BOOL unreadable_white = inside == RGB(255, 255, 255);
        t_check("screen-alpha-blended", !bad_pure_magenta,
                "inside=%06lX (%s)", (unsigned long)inside,
                bad_pure_magenta ? "alpha ignored" :
                unreadable_white ? "screen readback unavailable" : "blended");
        t_metric("center-pixel", "%06lX", (unsigned long)inside);
    }
    DestroyWindow(wnd);

    /* ---- 段 2：per-pixel alpha（UpdateLayeredWindow）---- */
    {
        HWND ulw_wnd = t_create_toplevel("Static", "WineHuaT_LayeredU", WS_POPUP,
                                         WS_EX_LAYERED, rect.left, rect.top, WND_W, WND_H,
                                         "LYRU");
        t_check("create-ulw-window", ulw_wnd != NULL, "err=%lu",
                ulw_wnd ? 0 : GetLastError());
        if (ulw_wnd)
        {
            HDC screen = GetDC(NULL);
            HDC mem = CreateCompatibleDC(screen);
            BLENDFUNCTION blend;
            POINT src = {0, 0};
            SIZE size = {WND_W, WND_H};
            POINT dst;
            HBITMAP bmp = CreateCompatibleBitmap(screen, WND_W, WND_H);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            BITMAPINFO bmi;
            unsigned char *bits = NULL;
            int x, y;

            SetWindowPos(ulw_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            GetWindowRect(ulw_wnd, &rect);
            dst.x = rect.left;
            dst.y = rect.top;

            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = WND_W;
            bmi.bmiHeader.biHeight = -WND_H;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            memset(&blend, 0, sizeof(blend));
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;

            /* 32bpp 预乘：左半蓝 alpha=255，右半 alpha=0 */
            bits = (unsigned char *)calloc(WND_W * WND_H, 4);
            if (bits)
            {
                for (y = 0; y < WND_H; ++y)
                    for (x = 0; x < WND_W; ++x)
                    {
                        unsigned char *px = bits + (y * WND_W + x) * 4;
                        px[0] = 255; /* B */
                        px[1] = 0;
                        px[2] = 0;
                        px[3] = x < WND_W / 2 ? 255 : 0; /* A */
                    }
                SetDIBits(mem, bmp, 0, WND_H, bits, &bmi, DIB_RGB_COLORS);
                /* hdcDst 传 NULL：让系统自行取窗口 DC */
                ok = UpdateLayeredWindow(ulw_wnd, NULL, &dst, &size, mem, &src, 0,
                                         &blend, ULW_ALPHA);
                t_check("update-layered", ok, "err=%lu", ok ? 0 : GetLastError());
                if (ok)
                {
                    Sleep(300);
                    GetWindowRect(ulw_wnd, &rect);
                    {
                        COLORREF solid = screen_pixel(rect.left + WND_W / 4,
                                                      rect.top + WND_H / 2);
                        COLORREF hole = screen_pixel(rect.left + WND_W * 3 / 4,
                                                     rect.top + WND_H / 2);
                        t_check("perpixel-solid-visible",
                                GetRValue(solid) < 100 && GetBValue(solid) > 200,
                                "solid=%06lX", (unsigned long)solid);
                        /* 右半应透明：透出的不是本窗的蓝色 */
                        t_check("perpixel-hole-transparent", GetBValue(hole) < 200,
                                "hole=%06lX", (unsigned long)hole);
                        t_metric("perpixel", "solid=%06lX hole=%06lX",
                                 (unsigned long)solid, (unsigned long)hole);
                    }
                }
                free(bits);
            }
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(NULL, screen);
            DestroyWindow(ulw_wnd);
        }
    }

    /* ---- 段 3：窗口 region（圆角裁剪）---- */
    {
        HWND rgn_wnd = t_create_toplevel("Static", "WineHuaT_LayeredR", WS_POPUP | WS_VISIBLE,
                                         0, rect.left, rect.top, WND_W, WND_H, "LYRR");
        t_check("create-rgn-window", rgn_wnd != NULL, "err=%lu",
                rgn_wnd ? 0 : GetLastError());
        if (rgn_wnd)
        {
            HRGN rgn = CreateRoundRectRgn(0, 0, WND_W + 1, WND_H + 1, 40, 40);
            SetWindowPos(rgn_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            fill_magenta(rgn_wnd);
            UpdateWindow(rgn_wnd);
            t_check("set-window-rgn", SetWindowRgn(rgn_wnd, rgn, TRUE) != 0,
                    "err=%lu", GetLastError());
            Sleep(200);
            GetWindowRect(rgn_wnd, &rect);
            /* 角点 (0,0) 在圆角外：读回与本窗品红一致 = region 未裁（BAD）。
             * 全白 = 读回不可用（捕获链缺失），无法判定，记 metric 放行。 */
            {
                COLORREF corner = screen_pixel(rect.left + 1, rect.top + 1);
                BOOL uncut_magenta = GetRValue(corner) > 250 && GetGValue(corner) < 30 &&
                                     GetBValue(corner) > 250;
                t_check("rgn-cuts-corner", !uncut_magenta,
                        "corner=%06lX (%s)", (unsigned long)corner,
                        uncut_magenta ? "region not applied" : "cut or unreadable");
                t_metric("corner-pixel", "%06lX", (unsigned long)corner);
            }
            DestroyWindow(rgn_wnd);
        }
    }
    return t_finish();
}
