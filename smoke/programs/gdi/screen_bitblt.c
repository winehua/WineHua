/* winehua_t_screen_bitblt — 跨窗口读屏（P1，收敛项②验收载荷）。
 * 判定规格见 docs/engineering/testing-programs.md §3.5。
 * 先红合法：捕获回灌落地前，读回半段应 FAIL；落地后转绿。
 * 失败特征：读回只有本进程内容或全黑 = 捕获回灌缺失。
 */
#include "../common/winehua_t_check.h"
#include <stdlib.h>

#define CELL 16
#define COLS 10
#define ROWS 6

static HWND wnd;

static void draw_checkerboard(HDC dc)
{
    int cx, cy;
    HBRUSH dark = CreateSolidBrush(RGB(20, 20, 20));
    HBRUSH light = CreateSolidBrush(RGB(230, 230, 230));
    RECT r;

    for (cy = 0; cy < ROWS; ++cy)
        for (cx = 0; cx < COLS; ++cx)
        {
            r.left = cx * CELL;
            r.top = cy * CELL;
            r.right = r.left + CELL;
            r.bottom = r.top + CELL;
            FillRect(dc, &r, ((cx + cy) & 1) ? light : dark);
        }
    DeleteObject(dark);
    DeleteObject(light);
}

static int cell_expect(int cx, int cy)
{
    return ((cx + cy) & 1) ? 230 : 20;
}

int main(int argc, char **argv)
{
    RECT rect;
    int hit = 0, miss = 0;
    int cx, cy;

    t_begin("winehua_t_screen_bitblt", argc, argv);

    wnd = t_create_toplevel("Static", "WineHuaT_ScrBlt", WS_POPUP | WS_VISIBLE, 0,
                            120, 120, COLS * CELL, ROWS * CELL, "CB");
    t_check("create-window", wnd != NULL, "err=%lu", wnd ? 0 : GetLastError());
    if (!wnd)
        return t_finish();
    TCHECK("set-topmost",
           (SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)));

    {
        HDC dc = GetDC(wnd);
        if (dc)
        {
            draw_checkerboard(dc);
            ReleaseDC(wnd, dc);
        }
    }
    UpdateWindow(wnd);
    Sleep(400); /* 等合成器把这一帧呈现到屏幕 */

    /* GetDC(NULL) 屏幕 DC + GetPixel 读回窗口区域：跨窗口捕获链 */
    GetWindowRect(wnd, &rect);
    {
        HDC screen = GetDC(NULL);
        t_check("screen-dc", screen != NULL, "screen dc");
        if (screen)
        {
            /* 采每格中心点，跳过边缘 2px 防 antialias */
            for (cy = 0; cy < ROWS; ++cy)
                for (cx = 0; cx < COLS; ++cx)
                {
                    COLORREF c = GetPixel(screen, rect.left + cx * CELL + CELL / 2,
                                          rect.top + cy * CELL + CELL / 2);
                    int expect = cell_expect(cx, cy);
                    int v = (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3;
                    if (abs(v - expect) <= 40)
                        ++hit;
                    else
                        ++miss;
                }
            t_metric("cells-hit", "%d", hit);
            t_metric("cells-miss", "%d", miss);
            t_check("screen-readback", hit >= COLS * ROWS * 9 / 10,
                    "hit=%d miss=%d of %d (capture backfeed required)",
                    hit, miss, COLS * ROWS);
            ReleaseDC(NULL, screen);
        }

        /* BitBlt 屏幕区域到内存 DC 再读：验证 BitBlt 路径同链路 */
        {
            HDC screen = GetDC(NULL);
            HDC mem = CreateCompatibleDC(screen);
            HBITMAP bmp = CreateCompatibleBitmap(screen, COLS * CELL, ROWS * CELL);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            BOOL ok = BitBlt(mem, 0, 0, COLS * CELL, ROWS * CELL, screen,
                             rect.left, rect.top, SRCCOPY);
            t_check("bitblt-from-screen", ok, "err=%lu", ok ? 0 : GetLastError());
            if (ok)
            {
                COLORREF c = GetPixel(mem, CELL / 2, CELL / 2);
                int v = (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3;
                t_check("bitblt-first-cell-dark", abs(v - 20) <= 40, "v=%d", v);
            }
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(NULL, screen);
        }
    }

    DestroyWindow(wnd);
    return t_finish();
}
