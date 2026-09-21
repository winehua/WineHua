/* Read-only window/queue probe. WM_NULL has no application action. */
#include <windows.h>
#include <stdio.h>
static FILE *out;
static unsigned count;
static BOOL CALLBACK visit(HWND hwnd, LPARAM unused)
{
    DWORD pid, tid, error;
    DWORD_PTR reply = 0;
    RECT rect;
    char title[256] = {0}, cls[128] = {0};
    GUITHREADINFO gui = {sizeof(gui)};
    ULONGLONG started;
    LRESULT result;
    (void)unused;
    if (!IsWindowVisible(hwnd)) return TRUE;
    tid = GetWindowThreadProcessId(hwnd, &pid);
    GetWindowRect(hwnd, &rect);
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetGUIThreadInfo(tid, &gui);
    SetLastError(0);
    started = GetTickCount64();
    result = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &reply);
    error = GetLastError();
    fprintf(out, "hwnd=%p pid=%lu tid=%lu rect=%ld,%ld,%ld,%ld enabled=%d iconic=%d alive=%lld error=%lu ms=%llu active=%p focus=%p capture=%p class=%s title=%s\n",
            hwnd, pid, tid, rect.left, rect.top, rect.right, rect.bottom,
            IsWindowEnabled(hwnd), IsIconic(hwnd), (long long)result, error,
            GetTickCount64() - started, gui.hwndActive, gui.hwndFocus,
            gui.hwndCapture, cls, title);
    return ++count < 40;
}
int main(void)
{
    out = fopen("C:\\window-response.txt", "w");
    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "tick=%llu foreground=%p\n", GetTickCount64(), GetForegroundWindow());
    EnumWindows(visit, 0);
    fclose(out);
    return 0;
}
