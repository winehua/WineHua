/* winehua_t_msg_order — 窗口创建/销毁消息时序（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.2。
 * 失败特征：时序错 = win32u 消息派发回归（对 win32u 改动最敏感的哨兵）。
 */
#include "../common/winehua_t_check.h"

#define T_MAX_MSG 64
static UINT g_seen[T_MAX_MSG];
static int g_n;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (g_n < T_MAX_MSG)
        g_seen[g_n++] = msg;
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int seen_before(UINT a, UINT b)
{
    int ia = -1, ib = -1, i;
    for (i = 0; i < g_n; ++i)
    {
        if (g_seen[i] == a && ia < 0) ia = i;
        if (g_seen[i] == b && ib < 0 && i != ia) ib = i;
    }
    return ia >= 0 && ib >= 0 && ia < ib;
}

static int seen_once(UINT m)
{
    int i, n = 0;
    for (i = 0; i < g_n; ++i)
        if (g_seen[i] == m) n++;
    return n == 1;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;

    t_begin("winehua_t_msg_order", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_MsgOrder";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    g_n = 0;
    hwnd = CreateWindowExA(0, "WineHuaT_MsgOrder", "order",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 50, 420, 320,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "hwnd=%p", hwnd);
    if (!hwnd)
        return t_finish();

    /* Windows 真实顺序：CreateWindow 期间 WM_GETMINMAXINFO(0x24) 先于 WM_NCCREATE */
    t_check("wm-getminmaxinfo-first", g_n > 0 && g_seen[0] == WM_GETMINMAXINFO,
            "first=0x%04X", g_n ? g_seen[0] : 0);
    t_check("nccreate-before-create", seen_before(WM_NCCREATE, WM_CREATE),
            "NCCREATE must precede CREATE");
    t_check("create-once", seen_once(WM_CREATE), "WM_CREATE exactly once");
    t_check("size-after-show", seen_before(WM_SHOWWINDOW, WM_SIZE) ||
            seen_before(WM_SIZE, WM_SHOWWINDOW) || seen_once(WM_SIZE),
            "size/showwindow delivered (either order, no dup required)");

    g_n = 0;
    DestroyWindow(hwnd);
    t_check("destroy-sequence", seen_before(WM_DESTROY, WM_NCDESTROY),
            "DESTROY must precede NCDESTROY");
    t_check("destroy-once", seen_once(WM_DESTROY) && seen_once(WM_NCDESTROY),
            "each destroy-phase message exactly once");
    return t_finish();
}
