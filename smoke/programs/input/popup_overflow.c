/* winehua_t_popup_overflow — x11 fusion 弹出层越界验证 (M4c-T3 手动验证
 * 探针)。失败特征：菜单项渲染在主窗 rect 之外（窗底缘以下）时点不中/不显
 * 示 = popup 子窗越界承载断。
 * 与 e2e_menu.c 的区别：主窗是「短窗」，TrackPopupMenu 弹在客户区底缘 →
 * 整条菜单都在主窗 rect 之外（垂直越界）；点击不由注入协议驱动（fusion+x11
 * 文件协议仅 byTitle 窗中心可用），由验证者在 OHOS 侧按 popup 子窗几何
 * uitest 点击，探针以 WM_COMMAND 自证命中第 2 项。
 * 判定规格 (2026-10-13 判定重连)：标准 smoke result 协议 —— job 以 std
 * argv 下发 --automation/--result（见 smoke/jobs/popup-overflow-x11.json），
 * checks[] 随 result 归档，result-json 判定器按终态 status 裁决；点选仍由
 * 人工 uitest 触发，无人点击 = FAIL（该 job 不进无人套件）。
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
static HMENU g_menu;            /* menu_thread 在 TrackPopupMenu 前填 */
static volatile LONG g_menu_up; /* TrackPopupMenu 即将进入 (几何测定门) */

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
    g_menu = menu;
    InterlockedExchange(&g_menu_up, 1); /* 越界几何测定就绪 */
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
     * 大于主窗底缘。几何断言移到菜单显示后按**实测矩形**做（见下）——
     * 旧 menu-below-window 拿这里的自算原点与窗框底自比：算术上恒假
     * （客户区底与窗框底差 3px 非客户边框，实测 159<162）且不测任何被测
     * 对象（拿代码自放的值自比）。 */
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

    /* 越界几何实测 (2026-10-13 重设计, 替换恒假自比)。两条实测规律
     * (host wine-10 + mingw throwaway harness, 2026-10-13; 真探针下发
     * smoke 构建同款 win32 API):
     * ① 主线程必须 pump: 菜单 WM_MEASUREITEM 投给 owner 窗（本线程）
     *    队列, 不泵 = 菜单项永不测量, GetMenuItemRect 给零矩形。
     * ② 跟踪中的 popup 菜单, GetMenuItemRect 返回 "项在菜单客户区的
     *    位置 + owner 客户区屏幕原点" —— 漏了弹窗实际屏幕位置, 需补
     *    偏移 Δ = 菜单窗(#32768)屏幕原点 − owner 客户区屏幕原点（两者
     *    都实测）。三数据点交叉验证: 修正矩形恰平铺菜单窗 rect（出入
     *    3px 边框）。若修正后仍不落于菜单窗 rect 内（wine 版本差异）,
     *    不信该值, 只记 raw —— 场景断言宁缺毋假。 */
    {
        RECT wr, item_raw, item = {0}, wmr = {0};
        POINT co = {0, 0};
        BOOL measured = FALSE;
        int waited;
        GetWindowRect(g_main, &wr);
        for (waited = 0; waited < 60 && !measured; ++waited)
        {
            pump(100);
            if (!g_menu_up || !GetMenuItemRect(g_main, g_menu, 1, &item_raw))
                continue;
            {
                HWND mw = FindWindowA("#32768", NULL); /* 单菜单跟踪, 无歧义 */
                DWORD pid = 0;
                if (mw)
                    GetWindowThreadProcessId(mw, &pid);
                if (!mw || pid != GetCurrentProcessId() || !IsWindowVisible(mw))
                    continue;
                GetWindowRect(mw, &wmr);
            }
            ClientToScreen(g_main, &co);
            item.left = item_raw.left + (wmr.left - co.x);
            item.top = item_raw.top + (wmr.top - co.y);
            item.right = item_raw.right + (wmr.left - co.x);
            item.bottom = item_raw.bottom + (wmr.top - co.y);
            measured = item.left >= wmr.left && item.top >= wmr.top &&
                       item.right <= wmr.right && item.bottom <= wmr.bottom;
        }
        t_metric("menu-window-rect", "%ld,%ld,%ld,%ld",
                 (long)wmr.left, (long)wmr.top, (long)wmr.right, (long)wmr.bottom);
        if (measured)
        {
            t_metric("menu-item2-rect", "%ld,%ld,%ld,%ld",
                     (long)item.left, (long)item.top, (long)item.right, (long)item.bottom);
            t_metric("menu-item2-center", "%ld,%ld",
                     (long)((item.left + item.right) / 2), (long)((item.top + item.bottom) / 2));
            /* 场景前置: 目标项（第 2 项）真实越出主窗底缘 = 本次点选测的
             * 就是"越界承载 + 越界命中"。不成立则这条 FAIL —— 场景没搭
             * 起来, 后面的 command 结论没有意义。 */
            t_check("item2-below-window", item.top >= wr.bottom,
                    "item2.top=%ld window.bottom=%ld", (long)item.top, (long)wr.bottom);
        }
        else
        {
            t_metric("menu-item2-rect", "unavailable (err=%lu)", GetLastError());
        }
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
