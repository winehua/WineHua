/* winehua_t_e2e_menu — 菜单路由（P2，手段 I+R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.15。
 * 失败特征：点不中/收不到 = popup 路由/子窗承载断（Pad 菜单回归哨兵）。
 * 协议：工作线程 TrackPopupMenu 弹在已知位置（TPM_LEFTALIGN），第 2 项的
 * 屏幕位置可预算 → host 注入点击 → WM_COMMAND == 第 2 项 ID。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 300
#define CLIENT_H 200
#define ORIGIN_X 60
#define ORIGIN_Y 50
#define MENU_ID_BASE 100
#define CLICK_ITEM 2 /* 注入第 2 项 */

static volatile LONG g_command_id;
static HWND g_main;
static POINT g_item_pt; /* 第 2 项中心（屏幕坐标） */
static POINT g_menu_origin; /* 菜单弹出原点（TPM_LEFTALIGN 的 x,y） */
static DWORD g_menu_thread_id;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_COMMAND)
    {
        g_command_id = (LONG)LOWORD(wparam);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI menu_thread(LPVOID arg)
{
    HMENU menu, popup;
    MSG msg;
    (void)arg;
    menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, MENU_ID_BASE + 1, "First");
    AppendMenuA(menu, MF_STRING, MENU_ID_BASE + 2, "Second");
    AppendMenuA(menu, MF_STRING, MENU_ID_BASE + 3, "Third");
    popup = menu;
    /* 菜单弹在 g_menu_origin（TPM_LEFTALIGN），第 2 项中心 =
     * 原点 + (30, 1.5 项高)——与主线程写入 request 的注入坐标同源 */
    SetForegroundWindow(g_main);
    TrackPopupMenu(popup, TPM_LEFTALIGN | TPM_LEFTBUTTON,
                   g_menu_origin.x, g_menu_origin.y, 0, g_main, NULL);
    /* 选择后 WM_COMMAND 已投递给 g_main；消息循环驱动 */
    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT)
            break;
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
    RECT frame = {0, 0, CLIENT_W, CLIENT_H};
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    int wait, item_h;

    t_begin("winehua_t_e2e_menu", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_E2EMenu";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    g_main = CreateWindowExA(0, "WineHuaT_E2EMenu", "menu",
                             WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                             ORIGIN_X, ORIGIN_Y,
                             frame.right - frame.left, frame.bottom - frame.top,
                             NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", g_main != NULL, "err=%lu", g_main ? 0 : GetLastError());
    if (!g_main)
        return t_finish();
    pump(200);

    /* 菜单弹在客户区左上（屏幕坐标），第 2 项中心 = 原点 + (30, 1.5 项高)。
     * 注入加 400ms 前置 sleep，等菜单线程完成 TrackPopupMenu 进入模态循环 */
    item_h = GetSystemMetrics(SM_CYMENU);
    if (item_h <= 0) item_h = 19;
    {
        POINT origin = {0, 0};
        ClientToScreen(g_main, &origin);
        g_menu_origin.x = origin.x;
        g_menu_origin.y = origin.y;
        g_item_pt.x = origin.x + 30;
        g_item_pt.y = origin.y + item_h + item_h / 2;
    }

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    f = fopen(req_path, "w");
    t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
    if (!f)
        return t_finish();
    fprintf(f, "{\"actions\":["
               "{\"type\":\"sleep\",\"ms\":400},"
               "{\"type\":\"click\",\"x\":%ld,\"y\":%ld}"
               "],\"actionGapMs\":200}",
            (long)g_item_pt.x, (long)g_item_pt.y);
    fclose(f);
    t_metric("menu-item-pt", "%ld,%ld", (long)g_item_pt.x, (long)g_item_pt.y);

    /* 工作线程弹菜单（阻塞），主线程泵消息收 WM_COMMAND */
    g_menu_thread_id = 0;
    {
        HANDLE thread = CreateThread(NULL, 0, menu_thread, NULL, 0,
                                     &g_menu_thread_id);
        CloseHandle(thread);
    }
    for (wait = 0; wait < 300; ++wait)
    {
        FILE *done = fopen(done_path, "r");
        if (done)
        {
            char buf[256];
            memset(buf, 0, sizeof(buf));
            fgets(buf, sizeof(buf), done);
            fclose(done);
            t_metric("inject-done", "%s", buf);
            break;
        }
        pump(100);
    }
    t_check("inject-done-seen", wait < 300, "waited %d00ms", wait);
    pump(1000);

    t_check("command-received", g_command_id != 0,
            "WM_COMMAND id=%ld", (long)g_command_id);
    t_check("command-item2", g_command_id == MENU_ID_BASE + CLICK_ITEM,
            "id=%ld expect %d", (long)g_command_id, MENU_ID_BASE + CLICK_ITEM);

    /* 关菜单线程 */
    PostThreadMessage(g_menu_thread_id, WM_QUIT, 0, 0);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(g_main);
    return t_finish();
}
