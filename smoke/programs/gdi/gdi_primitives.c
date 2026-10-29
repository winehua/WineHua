/* winehua_t_gdi_primitives — 离屏 DC 基本图元自绘自读（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.4。
 * 失败特征：错色/空缺 = GDI 光栅化或像素格式断。
 */
#include "../common/winehua_t_check.h"

static int read_pixel(HDC mem, int x, int y, COLORREF expect)
{
    COLORREF got = GetPixel(mem, x, y);
    return got == expect;
}

int main(int argc, char **argv)
{
    HDC screen, mem;
    HBITMAP bmp, old;
    BITMAP bm;
    int bpp, rc;

    t_begin("winehua_t_gdi_primitives", argc, argv);

    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, 200, 200);
    old = (HBITMAP)SelectObject(mem, bmp);
    t_check("create-memdc", mem != NULL && bmp != NULL && old != NULL, "mem=%p bmp=%p", mem, bmp);
    if (!mem || !bmp)
        return t_finish();

    GetObjectA(bmp, sizeof(bm), &bm);
    bpp = bm.bmBitsPixel;
    t_metric("bpp", "%d", bpp);
    t_check("bpp-32", bpp == 32, "bpp=%d", bpp);

    /* 背景刷白 */
    rc = PatBlt(mem, 0, 0, 200, 200, WHITENESS);
    t_check("clear-white", rc && read_pixel(mem, 10, 10, RGB(255, 255, 255)),
            "rc=%d pixel=%06lX", rc, GetPixel(mem, 10, 10));

    /* 纯色实心矩形 */
    {
        HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
        RECT r = {50, 50, 100, 100};
        HBRUSH old_brush = (HBRUSH)SelectObject(mem, brush);
        FillRect(mem, &r, brush);
        SelectObject(mem, old_brush);
        DeleteObject(brush);
        t_check("solid-rect", read_pixel(mem, 75, 75, RGB(255, 0, 0)) &&
                read_pixel(mem, 49, 75, RGB(255, 255, 255)),
                "inside=%06lX outside=%06lX", GetPixel(mem, 75, 75),
                GetPixel(mem, 49, 75));
    }

    /* 直线：水平黑线 y=150, x∈[20,180] */
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HPEN old_pen = (HPEN)SelectObject(mem, pen);
        MoveToEx(mem, 20, 150, NULL);
        LineTo(mem, 180, 150);
        SelectObject(mem, old_pen);
        DeleteObject(pen);
        t_check("line", read_pixel(mem, 100, 150, RGB(0, 0, 0)) &&
                read_pixel(mem, 100, 152, RGB(255, 255, 255)),
                "on=%06lX off=%06lX", GetPixel(mem, 100, 150), GetPixel(mem, 100, 152));
    }

    /* 椭圆：实心蓝，圆心 (150,40) 半径 20 */
    {
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 255));
        HBRUSH old_brush = (HBRUSH)SelectObject(mem, brush);
        HPEN old_pen = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, 130, 20, 170, 60);
        SelectObject(mem, old_pen);
        SelectObject(mem, old_brush);
        DeleteObject(brush);
        t_check("ellipse", read_pixel(mem, 150, 40, RGB(0, 0, 255)) &&
                read_pixel(mem, 150, 10, RGB(255, 255, 255)),
                "center=%06lX outside=%06lX", GetPixel(mem, 150, 40),
                GetPixel(mem, 150, 10));
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return t_finish();
}
