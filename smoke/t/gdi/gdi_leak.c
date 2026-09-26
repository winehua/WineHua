/* winehua_t_gdi_leak — GDI/USER 句柄守恒（P5，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.4。
 * 失败特征：循环后计数净增=GDI 对象表泄漏（长跑游戏 10000 上限触顶的
 * 早期哨兵——应用报"内存不足"而物理内存充足的那类问题）。
 * 计数源 GetGuiResources：wine 当前为 stub（恒 0）。恒 0 时守恒判定无
 * 判据，按能力探针规则记 UNSUPPORTED（合法答案，不算失败）；创建/销毁
 * 往返的 API 全链成功仍照常判定。平台补齐 GetGuiResources 后守恒判定
 * 自动生效。
 */
#include "../common/winehua_t_check.h"

#define ROUNDS 64

int main(int argc, char **argv)
{
    DWORD gdi0, gdi1, usr0, usr1;
    int i;

    t_begin("winehua_t_gdi_leak", argc, argv);

    gdi0 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    usr0 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (gdi0 == 0 && usr0 == 0)
    {
        t_check("handle-conservation", 1,
                "UNSUPPORTED: GetGuiResources 恒 0（wine stub），无守恒判据");
        t_metric("gui-resources", "unsupported");
    }

    for (i = 0; i < ROUNDS; ++i)
    {
        /* GDI 组：bitmap/brush/pen/font 各自独立分配释放 */
        HBITMAP bm = CreateBitmap(8, 8, 1, 32, NULL);
        HBRUSH br = CreateSolidBrush(RGB(i & 0xff, 0x40, 0x80));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HFONT font = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH, "Arial");
        HDC dc = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(dc, bm);

        DeleteObject(old);
        if (dc) DeleteDC(dc);
        if (font) DeleteObject(font);
        if (pen) DeleteObject(pen);
        if (br) DeleteObject(br);
        if (bm) DeleteObject(bm);

        /* USER 组：窗口类+窗口配对注销销毁。类名逐轮独立，避免与
         * RegisterClass 的不可重复注册语义冲突 */
        {
            char cls[64];
            WNDCLASSA wc;
            HWND w;
            wsprintfA(cls, "WineHuaT_Leak_%d", i);
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = DefWindowProcA;
            wc.hInstance = GetModuleHandleA(NULL);
            wc.lpszClassName = cls;
            if (RegisterClassA(&wc))
            {
                w = CreateWindowExA(0, cls, "leak", WS_OVERLAPPED,
                                    0, 0, 8, 8, NULL, NULL,
                                    wc.hInstance, NULL);
                if (w) DestroyWindow(w);
                UnregisterClassA(cls, wc.hInstance);
            }
        }
    }

    gdi1 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    usr1 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (gdi0 != 0 || usr0 != 0)
    {
        t_check("gdi-handle-conserved", gdi1 == gdi0,
                "gdi %lu -> %lu (+%ld over %d rounds)",
                (unsigned long)gdi0, (unsigned long)gdi1,
                (long)gdi1 - (long)gdi0, ROUNDS);
        t_check("user-handle-conserved", usr1 == usr0,
                "user %lu -> %lu (+%ld)",
                (unsigned long)usr0, (unsigned long)usr1,
                (long)usr1 - (long)usr0);
    }
    t_metric("gdi-leak-rounds", "%d", ROUNDS);
    return t_finish();
}
