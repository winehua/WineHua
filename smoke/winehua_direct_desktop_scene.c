/* Win32 control fixture for two independently rendered Direct DXVK windows.
 * PASS here covers window operations and child render results. Screenshots
 * and App GPU draw/fence logs separately establish visible composition. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static char result_path[MAX_PATH], run_id[128], test_id[128];
static FILE *steps;
static unsigned actions;
static HWND found_window;

static BOOL CALLBACK find_window(HWND hwnd, LPARAM pid)
{
    DWORD owner;
    char name[80];
    GetWindowThreadProcessId(hwnd, &owner);
    GetClassNameA(hwnd, name, sizeof(name));
    if (owner == (DWORD)pid && !strcmp(name, "WineD3DSwitchCubeWindow")) {
        found_window = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND wait_window(const PROCESS_INFORMATION *process)
{
    ULONGLONG deadline = GetTickCount64() + 20000;
    do {
        found_window = NULL;
        EnumWindows(find_window, process->dwProcessId);
        if (found_window) return found_window;
        if (WaitForSingleObject(process->hProcess, 0) == WAIT_OBJECT_0) return NULL;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    return NULL;
}

static void phase(const char *name, DWORD duration)
{
    MSG msg;
    ULONGLONG deadline = GetTickCount64() + duration;
    if (steps) {
        fprintf(steps, "%llu %s\n", GetTickCount64(), name);
        fflush(steps);
    }
    while (GetTickCount64() < deadline) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}

static BOOL place(HWND hwnd, int x, int y, int w, int h)
{
    RECT rect = {0, 0, w, h};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    if (!SetWindowPos(hwnd, HWND_TOP, x, y, rect.right - rect.left,
                      rect.bottom - rect.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED)) return FALSE;
    ++actions;
    return TRUE;
}

static LRESULT CALLBACK cover_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 12, 18, "GDI ABOVE DIRECT", 16);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static BOOL child_pass(const char *path)
{
    char buffer[4096];
    FILE *file = fopen(path, "rb");
    size_t length;
    if (!file) return FALSE;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[length] = 0;
    fclose(file);
    return strstr(buffer, "\"status\": \"PASS\"") != NULL;
}

static HWND alpha_cover(HINSTANCE instance)
{
    WNDCLASSA wc = {0};
    BITMAPINFO info = {0};
    void *pixels = NULL;
    HDC screen = GetDC(NULL), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap, old;
    HWND window;
    POINT destination = {200, 170}, source = {0, 0};
    SIZE size = {300, 160};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    unsigned x, y;
    BOOL updated;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = instance; wc.lpszClassName = "WineHuaDirectSceneAlpha";
    RegisterClassA(&wc);
    window = CreateWindowExA(WS_EX_LAYERED, wc.lpszClassName, "ARGB cover", WS_POPUP,
                              destination.x, destination.y, size.cx, size.cy, NULL, NULL, instance, NULL);
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!window || !bitmap || !pixels || !memory) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(NULL, screen);
        if (window) DestroyWindow(window);
        return NULL;
    }
    for (y = 0; y < (unsigned)size.cy; ++y)
        for (x = 0; x < (unsigned)size.cx; ++x)
            ((DWORD *)pixels)[y * size.cx + x] = x < 100 ? 0 : x < 200 ? 0x80008000 : 0xff00ff00;
    old = SelectObject(memory, bitmap);
    updated = UpdateLayeredWindow(window, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(NULL, screen);
    if (!updated) { DestroyWindow(window); return NULL; }
    ShowWindow(window, SW_SHOW); return window;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    PROCESS_INFORMATION children[2] = {{0}};
    STARTUPINFOA startup = {sizeof(startup)};
    HWND windows[2] = {0}, cover = NULL;
    WNDCLASSA wc = {0};
    HBRUSH brush = CreateSolidBrush(RGB(70, 220, 130));
    char exe[MAX_PATH], child_paths[2][MAX_PATH], line[4 * MAX_PATH];
    char step_path[MAX_PATH], temporary[MAX_PATH];
    const char *stage = "arguments";
    BOOL pass = FALSE, child_ok[2] = {FALSE, FALSE};
    int argc, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--result"))
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, result_path, sizeof(result_path), NULL, NULL);
        else if (!wcscmp(argv[i], L"--run-id"))
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, run_id, sizeof(run_id), NULL, NULL);
        else if (!wcscmp(argv[i], L"--test-id"))
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, test_id, sizeof(test_id), NULL, NULL);
    }
    if (argv) LocalFree(argv);
    if (!*result_path) goto done;
    snprintf(step_path, sizeof(step_path), "%s.steps.log", result_path);
    steps = fopen(step_path, "wb");
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (!strrchr(exe, '\\')) goto done;
    strcpy(strrchr(exe, '\\') + 1, "winehua_d3d_resize_cube.exe");
    stage = "spawn";
    for (i = 0; i < 2; ++i) {
        snprintf(child_paths[i], sizeof(child_paths[i]), "%s.%c.json", result_path, 'a' + i);
        DeleteFileA(child_paths[i]);
        snprintf(line, sizeof(line), "\"%s\" --d3d11 --automation --seconds 90 --result \"%s\" --run-id %s --test-id scene-%c",
                 exe, child_paths[i], run_id, 'a' + i);
        if (!CreateProcessA(exe, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &children[i]))
            goto done;
    }
    stage = "windows";
    for (i = 0; i < 2; ++i) if (!(windows[i] = wait_window(&children[i]))) goto done;
    phase("initial-render", 3000);
    stage = "overlap";
    if (!place(windows[0], 40, 60, 600, 400) || !place(windows[1], 300, 160, 600, 400)) goto done;
    phase(stage, 4000);
    stage = "move";
    if (!place(windows[1], 650, 280, 540, 360)) goto done;
    phase(stage, 4000);
    stage = "gdi-cover";
    wc.lpfnWndProc = cover_proc;
    wc.hInstance = instance;
    wc.hbrBackground = brush;
    wc.lpszClassName = "WineHuaDirectSceneCover";
    if (!RegisterClassA(&wc)) goto done;
    cover = CreateWindowA(wc.lpszClassName, "Opaque GDI cover", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          180, 140, 320, 160, NULL, NULL, instance, NULL);
    if (!cover) goto done;
    ++actions;
    phase(stage, 4000);
    stage = "minimize";
    DestroyWindow(cover); cover = NULL;
    stage = "alpha-cover";
    cover = alpha_cover(instance);
    if (!cover) goto done;
    ++actions;
    phase(stage, 4000);
    DestroyWindow(cover); cover = NULL;
    stage = "minimize";
    ShowWindow(windows[1], SW_MINIMIZE);
    if (!IsIconic(windows[1])) goto done;
    ++actions;
    phase(stage, 3000);
    stage = "restore";
    ShowWindow(windows[1], SW_RESTORE);
    if (IsIconic(windows[1]) || !place(windows[1], 360, 240, 540, 360)) goto done;
    phase(stage, 4000);
    stage = "destroy-b";
    PostMessageA(windows[1], WM_CLOSE, 0, 0);
    if (WaitForSingleObject(children[1].hProcess, 10000) != WAIT_OBJECT_0) goto done;
    windows[1] = NULL; ++actions;
    if (!place(windows[0], 120, 100, 700, 450)) goto done;
    phase(stage, 4000);
    stage = "fullscreen";
    SetWindowLongPtrA(windows[0], GWL_STYLE, WS_POPUP | WS_VISIBLE);
    if (!SetWindowPos(windows[0], HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                      SWP_FRAMECHANGED | SWP_SHOWWINDOW)) goto done;
    ++actions;
    phase(stage, 4000);
    stage = "windowed";
    SetWindowLongPtrA(windows[0], GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    if (!place(windows[0], 120, 100, 700, 450)) goto done;
    phase(stage, 3000);
    stage = "destroy-a";
    PostMessageA(windows[0], WM_CLOSE, 0, 0);
    if (WaitForSingleObject(children[0].hProcess, 10000) != WAIT_OBJECT_0) goto done;
    windows[0] = NULL; ++actions;
    child_ok[0] = child_pass(child_paths[0]);
    child_ok[1] = child_pass(child_paths[1]);
    pass = child_ok[0] && child_ok[1];
    phase("desktop-idle", 2000);
done:
    if (cover) DestroyWindow(cover);
    for (i = 0; i < 2; ++i) {
        if (windows[i]) PostMessageA(windows[i], WM_CLOSE, 0, 0);
        if (children[i].hProcess) {
            if (WaitForSingleObject(children[i].hProcess, 5000) == WAIT_TIMEOUT)
                TerminateProcess(children[i].hProcess, 1);
            CloseHandle(children[i].hThread);
            CloseHandle(children[i].hProcess);
        }
    }
    if (steps) fclose(steps);
    if (brush) DeleteObject(brush);
    if (*result_path) {
        FILE *file;
        snprintf(temporary, sizeof(temporary), "%s.tmp", result_path);
        if ((file = fopen(temporary, "wb"))) {
            fprintf(file, "{\"schemaVersion\":1,\"runId\":\"%s\",\"testId\":\"%s\",\"status\":\"%s\",\"stage\":\"%s\","
                          "\"message\":\"Win32 operations and child results; visible GPU composition requires screenshots and App logs\","
                          "\"windowActions\":%u,\"childA\":%s,\"childB\":%s}\n",
                    run_id, test_id, pass ? "PASS" : "FAIL", stage, actions,
                    child_ok[0] ? "true" : "false", child_ok[1] ? "true" : "false");
            fclose(file);
            MoveFileExA(temporary, result_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }
    return pass ? 0 : 1;
}
