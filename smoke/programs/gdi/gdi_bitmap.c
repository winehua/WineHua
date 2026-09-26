/* winehua_t_gdi_bitmap — DIB 与位块传输（P2，手段 P）。
 * 判定规格见 docs/engineering/testing-programs.md §3.4。
 * 失败特征：stride 错位 = DIB 布局断；缩放采样错 = StretchBlt 路径断。
 */
#include "../common/winehua_t_check.h"

#define W 64
#define H 48

static COLORREF src_pixel(int x, int y)
{
    return RGB(x * 4 % 256, y * 5 % 256, 128);
}

int main(int argc, char **argv)
{
    HDC mem_src, mem_dst;
    HBITMAP bmp_src, bmp_dst;
    BITMAPINFO bmi;
    unsigned char *bits_src = NULL, *bits_dst = NULL;
    BITMAP bm;
    int x, y;

    t_begin("winehua_t_gdi_bitmap", argc, argv);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    {
        HDC screen = GetDC(NULL);
        mem_src = CreateCompatibleDC(screen);
        mem_dst = CreateCompatibleDC(screen);
        /* 两个 DIB 的内存指针必须分开接——共用一个会被第二次调用覆盖 */
        bmp_src = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, (void **)&bits_src, NULL, 0);
        bmp_dst = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, (void **)&bits_dst, NULL, 0);
        ReleaseDC(NULL, screen);
    }
    t_check("create-dibs", bmp_src && bmp_dst && mem_src && mem_dst,
            "src=%p dst=%p", bmp_src, bmp_dst);
    if (!(bmp_src && bmp_dst))
        return t_finish();

    SelectObject(mem_src, bmp_src);
    SelectObject(mem_dst, bmp_dst);

    /* 源 DIB 直接写位（绕过 GDI 绘制，验证 DIB 布局与读回） */
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x)
        {
            COLORREF c = src_pixel(x, y);
            unsigned char *px = bits_src + (y * W + x) * 4;
            px[0] = GetBValue(c);
            px[1] = GetGValue(c);
            px[2] = GetRValue(c);
            px[3] = 0;
        }

    /* 内存 DIB 的 GetPixel 读回（DIB 布局哨兵） */
    {
        COLORREF c = GetPixel(mem_src, 10, 8);
        COLORREF e = src_pixel(10, 8);
        t_check("dib-readback", c == e, "got %06lX expect %06lX",
                (unsigned long)c, (unsigned long)e);
    }

    /* 1:1 BitBlt → 读回逐像素抽样 */
    TEXPR(BitBlt(mem_dst, 0, 0, W, H, mem_src, 0, 0, SRCCOPY));
    {
        int ok = 1;
        for (y = 0; y < H && ok; y += 7)
            for (x = 0; x < W && ok; x += 5)
                if (GetPixel(mem_dst, x, y) != src_pixel(x, y))
                    ok = 0;
        t_check("bitblt-1to1", ok, "pixel mismatch in sample grid");
    }

    /* 2:1 缩放 StretchBlt → 中心采样 ≈ 源对应区域均值 */
    TEXPR(StretchBlt(mem_dst, 0, 0, W / 2, H / 2, mem_src, 0, 0, W, H, SRCCOPY));
    {
        int ok = 1;
        for (y = 4; y < H / 2 - 4 && ok; y += 6)
            for (x = 4; x < W / 2 - 4 && ok; x += 6)
            {
                /* 目标 (x,y) ≈ 源 (2x..2x+1, 2y..2y+1) 平均；采样源 4 角平均 */
                COLORREF a = src_pixel(x * 2, y * 2);
                COLORREF b = src_pixel(x * 2 + 1, y * 2);
                COLORREF c = src_pixel(x * 2, y * 2 + 1);
                COLORREF dd = src_pixel(x * 2 + 1, y * 2 + 1);
                COLORREF got = GetPixel(mem_dst, x, y);
                int er = (GetRValue(a) + GetRValue(b) + GetRValue(c) + GetRValue(dd)) / 4;
                int eg = (GetGValue(a) + GetGValue(b) + GetGValue(c) + GetGValue(dd)) / 4;
                int eb = (GetBValue(a) + GetBValue(b) + GetBValue(c) + GetBValue(dd)) / 4;
                if (abs((int)GetRValue(got) - er) > 24 ||
                    abs((int)GetGValue(got) - eg) > 24 ||
                    abs((int)GetBValue(got) - eb) > 24)
                    ok = 0;
            }
        t_check("stretchblt-2to1", ok, "scaled sample out of tolerance");
    }

    /* 对象完整性：GetObject 尺寸回读 */
    if (GetObjectA(bmp_src, sizeof(bm), &bm) >= (int)sizeof(bm))
        t_check("bitmap-object", bm.bmWidth == W && bm.bmHeight == H,
                "GetObject %dx%d", bm.bmWidth, bm.bmHeight);
    else
        t_check("bitmap-object", 0, "GetObject failed");

    DeleteObject(bmp_src);
    DeleteObject(bmp_dst);
    DeleteDC(mem_src);
    DeleteDC(mem_dst);
    return t_finish();
}
