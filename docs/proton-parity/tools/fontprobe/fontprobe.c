/*
 * fontprobe.exe — locate which GDI text API / font combination fails under Wine on OHOS.
 *
 * Motivation (2026-09-12):
 *   Steam's vgui2\vgui_surfacelib\Win32Font.cpp asserts with
 *     "Couldn't get string length"
 *   and the official x86-64 client dies (steam.exe exit=1). The same message appears
 *   non-fatally in the 2023 client, so it is a long-standing Wine text-measurement defect.
 *   Wine's stderr is not surfaced anywhere readable, so this probe writes results to files.
 *
 * Output:
 *   C:\windows\temp\fontprobe.txt   (inside the prefix, readable from hdc shell)
 *   C:\fontprobe.txt
 *   Z:\fontprobe.txt                (shared storage, if Z: exists)
 *
 * Build (host):
 *   x86_64-w64-mingw32-gcc -O1 -municode -mwindows -o fontprobe.exe fontprobe.c -lgdi32 -luser32
 *
 * Run: launch through WineHua's game want channel with game_path pointing at this exe.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_out;

static void p(const char *fmt, ...)
{
    va_list ap;
    if (!g_out) return;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fputc('\n', g_out);
    fflush(g_out);
}

/* One measurement attempt: report success/failure + value + GetLastError. */
static void try_extent(const char *label, HDC hdc, const WCHAR *s, int len)
{
    SIZE size;
    DWORD err;
    BOOL ok;

    ZeroMemory(&size, sizeof(size));
    SetLastError(0xDEADBEEF);
    ok = GetTextExtentPoint32W(hdc, s, len, &size);
    err = GetLastError();
    p("  [%s] GetTextExtentPoint32W ok=%d cx=%ld cy=%ld lasterr=0x%08lX",
      label, ok ? 1 : 0, (long)size.cx, (long)size.cy, (unsigned long)err);

    if (!ok) {
        /* narrow further: which of the pieces fail? */
        INT fit = -1, dx[64];
        SIZE size2;
        ZeroMemory(&size2, sizeof(size2));
        SetLastError(0xDEADBEEF);
        ok = GetTextExtentExPointW(hdc, s, len, 0, NULL, NULL, &size2);
        err = GetLastError();
        p("      GetTextExtentExPointW(max=0,nfit=NULL,dxs=NULL) ok=%d cx=%ld lasterr=0x%08lX",
          ok ? 1 : 0, (long)size2.cx, (unsigned long)err);

        if (len <= 64) {
            ZeroMemory(dx, sizeof(dx));
            SetLastError(0xDEADBEEF);
            ok = GetTextExtentExPointW(hdc, s, len, 0, &fit, dx, &size2);
            err = GetLastError();
            p("      GetTextExtentExPointW(nfit,dxs)          ok=%d nfit=%d cx=%ld lasterr=0x%08lX",
              ok ? 1 : 0, fit, (long)size2.cx, (unsigned long)err);
        }

        {
            TEXTMETRICW tm;
            ZeroMemory(&tm, sizeof(tm));
            SetLastError(0xDEADBEEF);
            ok = GetTextMetricsW(hdc, &tm);
            err = GetLastError();
            p("      GetTextMetricsW                          ok=%d height=%ld ave=%ld lasterr=0x%08lX",
              ok ? 1 : 0, (long)tm.tmHeight, (long)tm.tmAveCharWidth, (unsigned long)err);
        }
    }
}

static void probe_face(const WCHAR *face, int height, int charset, const WCHAR *extra)
{
    LOGFONTW lf;
    HFONT hfont, old;
    HDC hdc;
    WCHAR actual[LF_FACESIZE];
    TEXTMETRICW tm;
    DWORD err;

    p("");
    p("=== face=\"%ls\" height=%d charset=%d ===", face, height, charset);

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = height;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = (BYTE)charset;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);

    SetLastError(0xDEADBEEF);
    hfont = CreateFontIndirectW(&lf);
    err = GetLastError();
    p(" CreateFontIndirectW -> %p lasterr=0x%08lX", (void *)hfont, (unsigned long)err);
    if (!hfont) return;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc) {
        p(" CreateCompatibleDC(NULL) FAILED lasterr=0x%08lX", (unsigned long)GetLastError());
        DeleteObject(hfont);
        return;
    }
    old = (HFONT)SelectObject(hdc, hfont);
    p(" SelectObject -> %p lasterr=0x%08lX", (void *)old, (unsigned long)GetLastError());

    actual[0] = 0;
    GetTextFaceW(hdc, LF_FACESIZE, actual);
    p(" GetTextFace -> \"%ls\"", actual);
    ZeroMemory(&tm, sizeof(tm));
    if (GetTextMetricsW(hdc, &tm))
        p(" metrics: height=%ld ascent=%ld descent=%ld ave=%ld pitch=%ld charset=%u",
          (long)tm.tmHeight, (long)tm.tmAscent, (long)tm.tmDescent,
          (long)tm.tmAveCharWidth, (long)tm.tmPitchAndFamily, (unsigned)tm.tmCharSet);
    else
        p(" GetTextMetricsW FAILED lasterr=0x%08lX", (unsigned long)GetLastError());

    try_extent("ascii", hdc, L"Steam", 5);
    try_extent("ascii-1", hdc, L"W", 1);
    try_extent("empty-0", hdc, L"", 0);
    try_extent("cn", hdc, L"\x767B\x5F55 Steam", 8);   /* 登录 Steam */
    try_extent("cjk-1", hdc, L"\x767B", 1);            /* 登 */
    if (extra) try_extent("extra", hdc, extra, lstrlenW(extra));

    /* glyph indices: a separate family of failures */
    {
        WORD glyphs[64];
        DWORD gerr;
        SetLastError(0xDEADBEEF);
        int n = GetGlyphIndicesW(hdc, L"\x767B\x5F55 Steam", 8, glyphs, 0);
        gerr = GetLastError();
        p("  GetGlyphIndicesW n=%d lasterr=0x%08lX glyphs=%u,%u,%u,%u,%u,%u,%u,%u",
          n, (unsigned long)gerr,
          n > 0 ? glyphs[0] : 0, n > 1 ? glyphs[1] : 0, n > 2 ? glyphs[2] : 0,
          n > 3 ? glyphs[3] : 0, n > 4 ? glyphs[4] : 0, n > 5 ? glyphs[5] : 0,
          n > 6 ? glyphs[6] : 0, n > 7 ? glyphs[7] : 0);
    }

    SelectObject(hdc, old);
    DeleteDC(hdc);
    DeleteObject(hfont);
}

static int CALLBACK enum_proc(const LOGFONTW *lf, const TEXTMETRICW *tm,
                              DWORD type, LPARAM lp)
{
    int *count = (int *)lp;
    if (*count < 40)
        p("  font[%d] type=0x%08lX face=\"%ls\" charset=%u height=%ld",
          *count, (unsigned long)type, lf->lfFaceName,
          (unsigned)lf->lfCharSet, (long)tm->tmHeight);
    (*count)++;
    return 1;
}

static void dump_font_enum(void)
{
    HDC hdc = CreateCompatibleDC(NULL);
    LOGFONTW lf;
    int count = 0;

    p("");
    p("=== EnumFontFamiliesExW (DEFAULT_CHARSET) ===");
    if (!hdc) { p(" no DC"); return; }
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(hdc, &lf, (FONTENUMPROCW)enum_proc, (LPARAM)&count, 0);
    p(" total families (printed first 40): %d", count);
    DeleteDC(hdc);
}

static void dump_system(void)
{
    OSVERSIONINFOW vi;
    char path[MAX_PATH];
    char temp[MAX_PATH];
    char sys[MAX_PATH];

    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    GetVersionExW(&vi);
    p("OS version %lu.%lu build %lu", (unsigned long)vi.dwMajorVersion,
      (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);

    GetWindowsDirectoryA(path, sizeof(path));
    p("Windows dir = %s", path);
    GetTempPathA(sizeof(temp), temp);
    p("TempPath    = %s", temp);
    GetSystemDirectoryA(sys, sizeof(sys));
    p("System dir  = %s", sys);
}

static int run(void)
{
    static const WCHAR *faces[] = {
        L"Lucida Console",
        L"Tahoma",
        L"Microsoft Sans Serif",
        L"Arial",
        L"Segoe UI",
        L"MS Shell Dlg",
        L"Courier New",
        L"Verdana",
        L"HarmonyOS Sans SC",
        L"Noto Sans Mono",
        L"Noto Sans",
        L"System",
        L"SimSun",
        L"Microsoft YaHei UI",
    };
    static const char *names[] = {
        "Lucida Console", "Tahoma", "Microsoft Sans Serif", "Arial", "Segoe UI",
        "MS Shell Dlg", "Courier New", "Verdana", "HarmonyOS Sans SC", "Noto Sans Mono",
        "Noto Sans", "System", "SimSun", "Microsoft YaHei UI",
    };
    int i;

    p("fontprobe - Wine/OHOS GDI text measurement probe");
    p("build: %s %s", __DATE__, __TIME__);
    dump_system();
    dump_font_enum();

    for (i = 0; i < (int)(sizeof(faces) / sizeof(faces[0])); i++)
        probe_face(faces[i], -16, DEFAULT_CHARSET, L"Steam\x2122 \x00A9");

    /* Steam's VGUI also uses a big font for the CEF window chrome */
    for (i = 0; i < 4; i++) {
        static const int heights[] = { -11, -12, -13, -24 };
        probe_face(L"Tahoma", heights[i], DEFAULT_CHARSET, NULL);
    }
    probe_face(L"Tahoma", -16, GB2312_CHARSET, NULL);
    probe_face(L"Tahoma", -16, SHIFTJIS_CHARSET, NULL);

    p("");
    p("=== display DC path (CreateDCW DISPLAY) ===");
    {
        HDC hdc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
        p(" CreateDCW(DISPLAY) -> %p lasterr=0x%08lX", (void *)hdc, (unsigned long)GetLastError());
        if (hdc) {
            try_extent("display-ascii", hdc, L"Steam", 5);
            try_extent("display-cn", hdc, L"\x767B\x5F55 Steam", 8);
            DeleteDC(hdc);
        }
    }

    p("");
    p("=== done ===");
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    (void)hInst; (void)hPrev; (void)cmd; (void)show;

    g_out = fopen("C:\\windows\\temp\\fontprobe.txt", "w");
    if (!g_out) g_out = fopen("C:\\fontprobe.txt", "w");
    if (!g_out) g_out = fopen("Z:\\fontprobe.txt", "w");
    if (!g_out) return 1;

    run();

    fclose(g_out);

    /* second copy on shared storage */
    {
        FILE *src = fopen("C:\\windows\\temp\\fontprobe.txt", "rb");
        FILE *dst = fopen("Z:\\fontprobe.txt", "wb");
        if (src && dst) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    return 0;
}
