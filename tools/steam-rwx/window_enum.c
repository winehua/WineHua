/* Read-only Win32 top-level window enumerator for the OHOS Wine session.
 *
 * Purpose: answer "does the Windows side still have a window, and is it
 * visible?" for the Steam/CEF processes when the OHOS screen shows only the
 * wallpaper + taskbar. It never touches Wine or the compositor state.
 *
 * Output: C:\windows\temp\window-enum.log (plus stdout), one line per window.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>

static FILE* g_log;

static const wchar_t* TitleOf(HWND w, wchar_t* buf, int cch)
{
    int n = GetWindowTextW(w, buf, cch);
    if (n <= 0) {
        buf[0] = L'\0';
    }
    return buf;
}

static void ClassOf(HWND w, wchar_t* buf, int cch)
{
    if (!GetClassNameW(w, buf, cch)) {
        buf[0] = L'\0';
    }
}

static void ExeOf(DWORD pid, wchar_t* buf, int cch)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    buf[0] = L'\0';
    if (!p) {
        return;
    }
    DWORD size = (DWORD)cch;
    if (!QueryFullProcessImageNameW(p, 0, buf, &size)) {
        buf[0] = L'\0';
    }
    CloseHandle(p);
}

static BOOL CALLBACK EnumTop(HWND w, LPARAM param)
{
    DWORD pid = 0;
    wchar_t title[256], cls[128], exe[MAX_PATH];
    RECT r = {0, 0, 0, 0};
    LONG_PTR style, exstyle;
    (void)param;

    GetWindowThreadProcessId(w, &pid);
    TitleOf(w, title, 256);
    ClassOf(w, cls, 128);
    ExeOf(pid, exe, MAX_PATH);
    GetWindowRect(w, &r);
    style = GetWindowLongPtrW(w, GWL_STYLE);
    exstyle = GetWindowLongPtrW(w, GWL_EXSTYLE);

    fprintf(g_log,
            "TOP hwnd=%p pid=%lu tid=%lu vis=%d icon=%d en=%d rect=%ld,%ld,%ld,%ld "
            "style=0x%08llx ex=0x%08llx class=%ls exe=%ls title=%ls\n",
            (void*)w, (unsigned long)pid, (unsigned long)GetWindowThreadProcessId(w, NULL),
            IsWindowVisible(w) ? 1 : 0, IsIconic(w) ? 1 : 0, IsWindowEnabled(w) ? 1 : 0,
            (long)r.left, (long)r.top, (long)r.right, (long)r.bottom,
            (unsigned long long)style, (unsigned long long)exstyle, cls, exe, title);
    return TRUE;
}

static BOOL CALLBACK EnumChild(HWND w, LPARAM param)
{
    DWORD parentPid = (DWORD)param;
    DWORD pid = 0;
    wchar_t title[256], cls[128];
    RECT r = {0, 0, 0, 0};
    GetWindowThreadProcessId(w, &pid);
    if (pid != parentPid) {
        return TRUE;
    }
    TitleOf(w, title, 256);
    ClassOf(w, cls, 128);
    GetWindowRect(w, &r);
    fprintf(g_log,
            "  CHILD hwnd=%p pid=%lu vis=%d rect=%ld,%ld,%ld,%ld class=%ls title=%ls\n",
            (void*)w, (unsigned long)pid, IsWindowVisible(w) ? 1 : 0,
            (long)r.left, (long)r.top, (long)r.right, (long)r.bottom, cls, title);
    return TRUE;
}

int main(int argc, char** argv)
{
    DWORD filterPid = 0;
    if (argc > 1) {
        filterPid = (DWORD)strtoul(argv[1], NULL, 10);
    }

    g_log = fopen("C:\\windows\\temp\\window-enum.log", "w");
    if (!g_log) {
        g_log = stdout;
    }
    fprintf(g_log, "window-enum start pid=%lu filter=%lu\n",
            (unsigned long)GetCurrentProcessId(), (unsigned long)filterPid);

    EnumWindows(EnumTop, 0);

    fprintf(g_log, "-- children of filter pid %lu --\n", (unsigned long)filterPid);
    if (filterPid) {
        EnumWindows(EnumChild, (LPARAM)filterPid);
    }

    fprintf(g_log, "window-enum done\n");
    if (g_log != stdout) {
        fclose(g_log);
    }
    return 0;
}
