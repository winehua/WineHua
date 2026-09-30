/* winehua_t_gdi_palette — 调色板翻译（P3，手段 P）。
 * 判定规格见 docs/engineering/testing-programs.md §3.4。
 * 失败特征：错色=调色板翻译断（老游戏类依赖）。
 * 协议：8 位 DIB section → SetDIBColorTable 写已知色 → bits 写索引 →
 * BitBlt 到内存 DC → GetPixel 读回映射 RGB。
 */
#include "../common/winehua_t_check.h"

int main(int argc, char **argv)
{
    BITMAPINFO bmi;
    HBITMAP dib, old;
    HDC screen, mem;
    void *bits;
    RGBQUAD table[256];
    int i;

    t_begin("winehua_t_gdi_palette", argc, argv);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 8;
    bmi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    t_check("create-dib", dib != NULL && bits != NULL, "err=%lu", GetLastError());
    if (!dib)
    {
        ReleaseDC(NULL, screen);
        return t_finish();
    }
    mem = CreateCompatibleDC(screen);
    old = SelectObject(mem, dib);

    /* 索引 1/2 写已知色，读回校验颜色表本身 */
    memset(table, 0, sizeof(table));
    table[1].rgbRed = 0x11; table[1].rgbGreen = 0x22; table[1].rgbBlue = 0x33;
    table[2].rgbRed = 0xAA; table[2].rgbGreen = 0xBB; table[2].rgbBlue = 0xCC;
    UINT set = SetDIBColorTable(mem, 0, 256, table);
    t_check("set-color-table", set == 256, "set=%u", set);
    {
        RGBQUAD back[3];
        memset(back, 0, sizeof(back));
        UINT got = GetDIBColorTable(mem, 0, 3, back);
        t_check("color-table-roundtrip", got == 3 &&
                back[1].rgbRed == 0x11 && back[1].rgbGreen == 0x22 &&
                back[1].rgbBlue == 0x33 &&
                back[2].rgbRed == 0xAA && back[2].rgbGreen == 0xBB &&
                back[2].rgbBlue == 0xCC,
                "got=%u (%02x%02x%02x)(%02x%02x%02x)", got,
                back[1].rgbRed, back[1].rgbGreen, back[1].rgbBlue,
                back[2].rgbRed, back[2].rgbGreen, back[2].rgbBlue);
    }

    /* bits 写索引 → GetPixel 读映射色（COLORREF = 0x00BBGGRR） */
    memset(bits, 0, 16 * 16);
    ((unsigned char *)bits)[0 * 16 + 0] = 1;
    ((unsigned char *)bits)[0 * 16 + 1] = 2;
    GdiFlush();
    {
        COLORREF c1 = GetPixel(mem, 0, 0);
        COLORREF c2 = GetPixel(mem, 1, 0);
        t_check("palette-map-idx1", c1 == RGB(0x11, 0x22, 0x33),
                "c1=%08lx expect %08lx", (unsigned long)c1,
                (unsigned long)RGB(0x11, 0x22, 0x33));
        t_check("palette-map-idx2", c2 == RGB(0xAA, 0xBB, 0xCC),
                "c2=%08lx expect %08lx", (unsigned long)c2,
                (unsigned long)RGB(0xAA, 0xBB, 0xCC));
    }

    /* DIB→DIB 拷贝：目标表与源表一致时，索引色经 blt 保持映射不变
     * （8bpp blt 经 GDI 颜色匹配，跨不同表的索引不保真属合法行为） */
    {
        HBITMAP dib2 = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HDC mem2 = CreateCompatibleDC(screen);
        HBITMAP old2 = SelectObject(mem2, dib2);
        RGBQUAD t2[256];
        memset(t2, 0, sizeof(t2));
        t2[1].rgbRed = 0x11; t2[1].rgbGreen = 0x22; t2[1].rgbBlue = 0x33;
        t2[2].rgbRed = 0xAA; t2[2].rgbGreen = 0xBB; t2[2].rgbBlue = 0xCC;
        SetDIBColorTable(mem2, 0, 256, t2);
        BitBlt(mem2, 0, 0, 16, 16, mem, 0, 0, SRCCOPY);
        GdiFlush();
        COLORREF c = GetPixel(mem2, 1, 0);
        t_check("palette-map-after-blt", c == RGB(0xAA, 0xBB, 0xCC),
                "c=%08lx", (unsigned long)c);
        SelectObject(mem2, old2);
        DeleteObject(dib2);
        DeleteDC(mem2);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(NULL, screen);
    return t_finish();
}
