/* winehua_t_popup_overflow — x11 fusion 弹出层越界验证 (M4c-T3 手动验证
 * 探针)。失败特征：菜单项渲染在主窗 rect 之外（窗底缘以下）时点不中/不显
 * 示 = popup 子窗越界承载断。
 * 与 e2e_menu.c 的区别：主窗是「短窗」，TrackPopupMenu 弹在客户区底缘 →
 * 整条菜单都在主窗 rect 之外（垂直越界）；点击不由注入协议驱动（fusion+x11
 * 文件协议仅 byTitle 窗中心可用），由验证者在 OHOS 侧按 popup 子窗几何
 * uitest 点击，探针以 WM_COMMAND 自证命中第 2 项。
 * 判定规格：无（T4 接入判定器时补）；本探针 checks 写标准 result 文件。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 300
#define CLIENT_H 80  /* 短窗: 菜单弹在底缘即越出窗界 */
#define ORIGIN_X 60
#define ORIGIN_Y 50
#define MENU_ID_BASE 200
#define EXPECT_ITEM (MENU_ID_BASE + 2)

static volatile LONG g_command_id;
static volatile LONG g_owner_clicked; /* 主窗先点一下（真实流: 菜单栏/长按
                                      * 都先有 owner 指针交互）再开菜单 */
static HWND g_main;
static POINT g_menu_origin; /* 菜单弹出原点（客户区左下角, 屏幕坐标） */
static DWORD g_menu_thread_id;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam)
{
    if (msg == WM_COMMAND)
    {
        g_command_id = (LONG)LOWORD(wparam);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN)
    {
        g_owner_clicked = 1;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI menu_thread(LPVOID arg)
{
    HMENU menu;
    MSG msg;
    (void)arg;
    menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, MENU_ID_BASE + 1, "Alpha");
    AppendMenuA(menu, MF_STRING, MENU_ID_BASE + 2, "Beta");
    AppendMenuA(menu, MF_STRING, MENU_ID_BASE + 3, "Gamma");
    /* 与 e2e_menu 同款: 菜单线程 TrackPopupMenu 前置 SetForegroundWindow
     * (非前台线程跟踪菜单时 wine 侧事件归属会变, 菜单项命中失效) */
    SetForegroundWindow(g_main);
    /* 弹在客户区左下角: 菜单自窗底缘向下展开, 全部项在主窗 rect 之外 */
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_LEFTBUTTON,
                   g_menu_origin.x, g_menu_origin.y, 0, g_main, NULL);
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
    POINT client_bl;

    t_begin("winehua_t_popup_overflow", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_PopupOverflow";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    g_main = CreateWindowExA(0, "WineHuaT_PopupOverflow", "popup-overflow",
                             WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                             ORIGIN_X, ORIGIN_Y,
                             frame.right - frame.left, frame.bottom - frame.top,
                             NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", g_main != NULL, "err=%lu", g_main ? 0 : GetLastError());
    if (!g_main)
        return t_finish();
    pump(200);

    /* 菜单原点 = 客户区左下角（越界形态）。菜单向下展开 → 每一项的 y 都
     * 大于主窗底缘；第 2 项中心 ≈ 原点 + (30, 1.5 项高)。 */
    client_bl.x = 0;
    client_bl.y = CLIENT_H;
    ClientToScreen(g_main, &client_bl);
    g_menu_origin = client_bl;
    t_metric("menu-origin", "%ld,%ld", (long)g_menu_origin.x, (long)g_menu_origin.y);
    {
        RECT wr;
        GetWindowRect(g_main, &wr);
        t_metric("window-rect", "%ld,%ld,%ld,%ld",
                 (long)wr.left, (long)wr.top, (long)wr.right, (long)wr.bottom);
        t_check("menu-below-window", g_menu_origin.y >= wr.bottom,
                "menu_y=%ld bottom=%ld", (long)g_menu_origin.y, (long)wr.bottom);
    }

    /* 两段式（复刻真实流）: 验证者先点主窗（建立 owner 指针焦点, 等价
     * notepad 点菜单栏）→ 探针开菜单 → 验证者点越界菜单项。 */
    {
        int waited;
        for (waited = 0; waited < 600 && !g_owner_clicked; ++waited)
            pump(100);
        t_metric("owner-click-waited-ms", "%d", waited * 100);
        t_check("owner-clicked", g_owner_clicked != 0, "waited %d00ms", waited);
    }

    g_menu_thread_id = 0;
    {
        HANDLE thread = CreateThread(NULL, 0, menu_thread, NULL, 0,
                                     &g_menu_thread_id);
        CloseHandle(thread);
    }
    /* 菜单保持 150s 等验证者 uitest 点击（M4a 经验: 菜单延迟给足）; 命中即
     * 收 WM_COMMAND 提前结束。 */
    {
        int waited;
        for (waited = 0; waited < 1500 && !g_command_id; ++waited)
            pump(100);
        t_metric("waited-ms", "%d", waited * 100);
    }
    t_check("command-received", g_command_id != 0,
            "WM_COMMAND id=%ld", (long)g_command_id);
    t_check("command-item2", g_command_id == EXPECT_ITEM,
            "id=%ld expect %d", (long)g_command_id, EXPECT_ITEM);

    PostThreadMessage(g_menu_thread_id, WM_QUIT, 0, 0);
    pump(200);
    DestroyWindow(g_main);
    return t_finish();
}
