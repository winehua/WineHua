/* winehua_t_shell_dialogs — 公共对话框（P2，手段 I+R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.18。
 * 失败特征：对话框不弹/选不中 = comdlg32/子窗承载断。
 * 协议：预置文件 → 写 request（等对话框弹出 → type 文件名 → Enter）→
 * 工作线程调 GetOpenFileNameA → 返回路径比对。
 * 文件名只含字母数字与 _/.（编排 type 支持），初始目录 C:\smoke 省去路径。
 */
#include "../common/winehua_t_check.h"
#include <commdlg.h>

#define ORIGIN_X 70
#define ORIGIN_Y 60
#define TARGET_NAME "winehua_t_dialog_target.json"

static char g_picked[MAX_PATH];
static volatile LONG g_dialog_done; /* 1=OK 2=cancel */
static OPENFILENAMEA g_ofn;
static char g_file[MAX_PATH];

static DWORD WINAPI dialog_thread(LPVOID arg)
{
    memset(&g_ofn, 0, sizeof(g_ofn));
    g_ofn.lStructSize = sizeof(g_ofn);
    g_ofn.hwndOwner = (HWND)arg;
    g_ofn.lpstrFile = g_file;
    g_ofn.nMaxFile = sizeof(g_file);
    g_ofn.lpstrInitialDir = "C:\\smoke";
    g_ofn.lpstrFilter = "JSON\0*.json\0All\0*.*\0";
    g_ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&g_ofn))
    {
        lstrcpynA(g_picked, g_file, MAX_PATH);
        InterlockedExchange(&g_dialog_done, 1);
    }
    else
    {
        g_picked[0] = '\0';
        InterlockedExchange(&g_dialog_done, 2);
    }
    return 0;
}

static void pump(int ms)
{
    MSG msg;
    DWORD until = GetTickCount() + (DWORD)ms;
    while (GetTickCount() < until)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(20);
    }
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *marker, *f;
    int wait;

    t_begin("winehua_t_shell_dialogs", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_ShellDlg";
    TCHECK("register-class", RegisterClassA(&wc) != 0);
    hwnd = CreateWindowExA(0, "WineHuaT_ShellDlg", "dlg",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y, 400, 300,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    pump(200);

    /* 预置可选文件 */
    marker = fopen("C:\\smoke\\" TARGET_NAME, "w");
    if (marker)
    {
        fprintf(marker, "{}");
        fclose(marker);
    }

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    f = fopen(req_path, "w");
    t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
    if (!f)
        return t_finish();
    /* 对话框弹出（comdlg32 冷启动需 ~2s，sleep 给足）→ 点击对话框（建立
     * 键盘 enter 目标；点击点取居中对话框上部 = 文件名编辑框一带）→
     * 键入 → Enter */
    {
        int sx = GetSystemMetrics(SM_CXSCREEN);
        int sy = GetSystemMetrics(SM_CYSCREEN);
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"sleep\",\"ms\":2000},"
                   "{\"type\":\"click\",\"x\":%d,\"y\":%d},"
                   "{\"type\":\"type\",\"text\":\"" TARGET_NAME "\"},"
                   "{\"type\":\"key\",\"code\":28}"
                   "],\"actionGapMs\":150}",
                sx / 2, sy / 2 - 80);
        t_metric("dialog-click-pt", "%d,%d (screen %d,%d)", sx / 2, sy / 2 - 80, sx, sy);
    }
    fclose(f);

    {
        HANDLE thread = CreateThread(NULL, 0, dialog_thread, hwnd, 0, NULL);
        CloseHandle(thread);
    }
    for (wait = 0; wait < 400 && !g_dialog_done; ++wait)
    {
        FILE *done = fopen(done_path, "r");
        if (done)
        {
            char buf[256];
            memset(buf, 0, sizeof(buf));
            fgets(buf, sizeof(buf), done);
            fclose(done);
            t_metric("inject-done", "%s", buf);
        }
        pump(100);
    }
    pump(1500);
    t_check("dialog-completed", g_dialog_done != 0, "done=%ld", (long)g_dialog_done);
    t_check("picked-path", g_picked[0] != '\0', "picked empty (cancel or failure)");
    if (g_picked[0])
        t_check("picked-target", strstr(g_picked, TARGET_NAME) != NULL,
                "picked='%s'", g_picked);
    t_metric("picked", "%s", g_picked);

    DeleteFileA("C:\\smoke\\" TARGET_NAME);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
