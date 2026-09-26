/* winehua_t_clip_formats — 剪贴板格式枚举（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.6。
 * 失败特征：缺格式 = 格式转换层断。
 */
#include "../common/winehua_t_check.h"

int main(int argc, char **argv)
{
    HANDLE text, wtext, bitmap;
    const char *ansi = "WineHuaT-formats-42";
    const wchar_t *wide = L"WineHuaT-宽字符-42";

    t_begin("winehua_t_clip_formats", argc, argv);

    TCHECK("open", OpenClipboard(NULL));
    if (!OpenClipboard(NULL))
        return t_finish();
    TEXPR(EmptyClipboard());

    /* CF_TEXT */
    text = GlobalAlloc(GMEM_MOVEABLE, lstrlenA(ansi) + 1);
    if (text)
    {
        memcpy(GlobalLock(text), ansi, lstrlenA(ansi) + 1);
        GlobalUnlock(text);
        t_check("set-cf-text", SetClipboardData(CF_TEXT, text) != NULL, "err=%lu", GetLastError());
    }
    /* CF_UNICODETEXT */
    wtext = GlobalAlloc(GMEM_MOVEABLE, (lstrlenW(wide) + 1) * sizeof(wchar_t));
    if (wtext)
    {
        memcpy(GlobalLock(wtext), wide, (lstrlenW(wide) + 1) * sizeof(wchar_t));
        GlobalUnlock(wtext);
        t_check("set-cf-wtext", SetClipboardData(CF_UNICODETEXT, wtext) != NULL,
                "err=%lu", GetLastError());
    }
    /* CF_BITMAP：单色 8x8 兼容位图 */
    {
        HDC screen = GetDC(NULL);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, 8, 8);
        if (bmp)
        {
            HBRUSH brush = CreateSolidBrush(RGB(10, 200, 20));
            RECT r = {0, 0, 8, 8};
            HGDIOBJ old = SelectObject(mem, bmp);
            FillRect(mem, &r, brush);
            SelectObject(mem, old);
            DeleteObject(brush);
            bitmap = bmp;
            t_check("set-cf-bitmap", SetClipboardData(CF_BITMAP, bitmap) != NULL,
                    "err=%lu", GetLastError());
        }
        else
            t_check("make-bitmap", 0, "CreateCompatibleBitmap failed");
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }

    /* 枚举：三格式都应在链上 */
    {
        UINT fmt = 0;
        int has_text = 0, has_wtext = 0, has_bitmap = 0, total = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0)
        {
            total++;
            if (fmt == CF_TEXT) has_text = 1;
            if (fmt == CF_UNICODETEXT) has_wtext = 1;
            if (fmt == CF_BITMAP) has_bitmap = 1;
        }
        t_metric("formats-enumerated", "%d", total);
        t_check("enum-all-three", has_text && has_wtext && has_bitmap,
                "text=%d wtext=%d bitmap=%d (total %d)",
                has_text, has_wtext, has_bitmap, total);
    }

    /* 各格式独立取回校验 */
    {
        HANDLE got = GetClipboardData(CF_TEXT);
        t_check("get-cf-text", got && !lstrcmpA((const char *)GlobalLock(got), ansi),
                "got=%s", got ? (const char *)GlobalLock(got) : "(null)");
        if (got) GlobalUnlock(got);
        got = GetClipboardData(CF_UNICODETEXT);
        if (got)
        {
            const wchar_t *s = (const wchar_t *)GlobalLock(got);
            t_check("get-cf-wtext", s && !lstrcmpW(s, wide), "wtext match");
            GlobalUnlock(got);
        }
        else
            t_check("get-cf-wtext", 0, "null");
        got = GetClipboardData(CF_BITMAP);
        t_check("get-cf-bitmap", got != NULL, "bitmap handle");
    }

    CloseClipboard();
    return t_finish();
}
