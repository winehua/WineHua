/*
 * Read-only window/queue responsiveness probe (extended 2026-09-20).
 *
 * Same contract as window-response.c (WM_NULL only, no application action), plus:
 *   - IsHungAppWindow() per window (Windows' own hang verdict)
 *   - a second WM_NULL without SMTO_ABORTIFHUNG to measure the real reply time
 *   - thread identity (pid/tid) plus GUI thread active/focus/capture state
 *   - a stdout copy so the result also lands in the child's captured stderr
 *
 * Why: the tablet investigation needs "which Windows thread fails to pump, and
 * does Windows itself consider it hung" in the *same* generation as the
 * compositor/hilog evidence, without deploying any behaviour change.
 */
#include <windows.h>
#include <stdio.h>

static FILE *out;
static unsigned count;

static BOOL CALLBACK visit(HWND hwnd, LPARAM unused)
{
    DWORD pid = 0, tid = 0, error1 = 0, error2 = 0;
    DWORD_PTR reply1 = 0, reply2 = 0;
    RECT rect = {0, 0, 0, 0};
    char title[256] = {0}, cls[128] = {0};
    GUITHREADINFO gui;
    ULONGLONG started;
    LRESULT result1, result2;
    unsigned hung;

    (void)unused;
    if (!IsWindowVisible(hwnd)) return TRUE;

    tid = GetWindowThreadProcessId(hwnd, &pid);
    GetWindowRect(hwnd, &rect);
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    ZeroMemory(&gui, sizeof(gui));
    gui.cbSize = sizeof(gui);
    GetGUIThreadInfo(tid, &gui);
    hung = IsHungAppWindow(hwnd) ? 1u : 0u;

    SetLastError(0);
    started = GetTickCount64();
    result1 = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                                  SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &reply1);
    error1 = GetLastError();
    DWORD_PTR ms1 = (DWORD_PTR)(GetTickCount64() - started);

    SetLastError(0);
    started = GetTickCount64();
    result2 = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 1000, &reply2);
    error2 = GetLastError();
    DWORD_PTR ms2 = (DWORD_PTR)(GetTickCount64() - started);

    fprintf(out,
            "hwnd=%p pid=%lu tid=%lu rect=%ld,%ld,%ld,%ld enabled=%d iconic=%d hung=%u "
            "abort=%lld err=%lu ms=%llu noblock=%lld err=%lu ms=%llu "
            "active=%p focus=%p capture=%p class=%s title=%s\n",
            hwnd, pid, tid, rect.left, rect.top, rect.right, rect.bottom,
            IsWindowEnabled(hwnd), IsIconic(hwnd), hung,
            (long long)result1, error1, (unsigned long long)ms1,
            (long long)result2, error2, (unsigned long long)ms2,
            gui.hwndActive, gui.hwndFocus, gui.hwndCapture, cls, title);
    fflush(out);
    printf("WINRESP hwnd=%p pid=%lu tid=%lu hung=%u abort=%lld err=%lu ms=%llu "
           "noblock=%lld err=%lu ms=%llu class=%s\n",
           hwnd, pid, tid, hung, (long long)result1, error1,
           (unsigned long long)ms1, (long long)result2, error2,
           (unsigned long long)ms2, cls);
    fflush(stdout);
    return ++count < 40;
}

int main(void)
{
    out = fopen("C:\\window-response2.txt", "w");
    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "tick=%llu foreground=%p\n", GetTickCount64(), GetForegroundWindow());
    EnumWindows(visit, 0);
    fprintf(out, "windows=%u\n", count);
    fclose(out);
    printf("WINRESP done windows=%u\n", count);
    return 0;
}
