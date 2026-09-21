/* Bounded, read-only snapshot of Wine top-level windows for local diagnosis. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

static FILE *output;

static BOOL CALLBACK record_window(HWND window, LPARAM unused)
{
    WCHAR title[512], class_name[128];
    char title_utf8[2048], class_utf8[512];
    DWORD pid = 0;
    RECT rect = {0};
    (void)unused;
    GetWindowTextW(window, title, 512);
    GetClassNameW(window, class_name, 128);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, title_utf8, sizeof(title_utf8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, class_name, -1, class_utf8, sizeof(class_utf8), NULL, NULL);
    GetWindowThreadProcessId(window, &pid);
    GetWindowRect(window, &rect);
    fprintf(output, "hwnd=%p pid=%lu visible=%d iconic=%d owner=%p rect=%ld,%ld,%ld,%ld class=%s title=%s\n",
            window, pid, IsWindowVisible(window), IsIconic(window), GetWindow(window, GW_OWNER),
            rect.left, rect.top, rect.right, rect.bottom, class_utf8, title_utf8);
    return TRUE;
}

int main(void)
{
    output = fopen("C:\\windows\\temp\\winehua-window-snapshot.log", "wb");
    if (!output) return 1;
    fprintf(output, "foreground=%p tick=%llu\n", GetForegroundWindow(), GetTickCount64());
    EnumWindows(record_window, 0);
    fclose(output);
    return 0;
}
