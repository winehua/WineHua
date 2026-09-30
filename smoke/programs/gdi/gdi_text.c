/* winehua_t_gdi_text — 文本绘制与度量（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.4。
 * 失败特征：中文空缺 = 字体扫描/locale 断；extent=0 = 文本度量断。
 */
#include "../common/winehua_t_check.h"

static int any_nonwhite(HDC mem, int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = y0; y < y1; y += 2)
        for (x = x0; x < x1; x += 2)
            if (GetPixel(mem, x, y) != RGB(255, 255, 255))
                return 1;
    return 0;
}

int main(int argc, char **argv)
{
    HDC screen, mem;
    HBITMAP bmp, old;
    SIZE extent_a, extent_w;
    BOOL rc;

    t_begin("winehua_t_gdi_text", argc, argv);

    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, 300, 120);
    old = (HBITMAP)SelectObject(mem, bmp);
    t_check("create-memdc", mem != NULL && bmp != NULL, "mem=%p", mem);
    if (!mem || !bmp)
        return t_finish();
    PatBlt(mem, 0, 0, 300, 120, WHITENESS);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(0, 0, 0));

    /* 英文度量：尺寸为正且随字号增大 */
    rc = GetTextExtentPoint32A(mem, "Hello", 5, &extent_a);
    t_check("extent-ascii", rc && extent_a.cx > 0 && extent_a.cy > 0,
            "%ldx%ld", (long)extent_a.cx, (long)extent_a.cy);
    {
        HFONT big = CreateFontA(48, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                0, 0, CLEARTYPE_QUALITY, 0, "Arial");
        HFONT old_font = (HFONT)SelectObject(mem, big);
        SIZE extent_big;
        GetTextExtentPoint32A(mem, "Hello", 5, &extent_big);
        t_check("extent-scales", extent_big.cy > extent_a.cy,
                "small=%ld big=%ld", (long)extent_a.cy, (long)extent_big.cy);
        SelectObject(mem, old_font);
        DeleteObject(big);
    }

    /* 中文绘制：字形非空 + 宽字符度量 */
    {
        HFONT font = CreateFontW(24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"SimSun");
        HFONT old_font = (HFONT)SelectObject(mem, font);
        const wchar_t *cn = L"测试中文";
        TextOutW(mem, 10, 40, cn, 4);
        t_check("draw-chinese", any_nonwhite(mem, 10, 40, 120, 80),
                "chinese glyphs missing");
        rc = GetTextExtentPoint32W(mem, cn, 4, &extent_w);
        t_check("extent-chinese", rc && extent_w.cx >= 4 * 10,
                "%ldx%ld", (long)extent_w.cx, (long)extent_w.cy);
        t_metric("chinese_extent", "%ldx%ld", (long)extent_w.cx, (long)extent_w.cy);
        SelectObject(mem, old_font);
        DeleteObject(font);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return t_finish();
}
