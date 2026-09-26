/* winehua_t_win_minimize — 最小化/还原循环与 -32000 哨兵（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 失败特征：还原尺寸错 = 合成器猜尺寸/SC_RESTORE 握手断。
 */
#include "../common/winehua_t_check.h"

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static BOOL check_rect(HWND hwnd, int x, int y, int w, int h, const char *name)
{
    RECT r;
    if (!GetWindowRect(hwnd, &r))
    {
        t_check(name, 0, "GetWindowRect failed");
        return FALSE;
    }
    t_check(name, r.left >= x - 4 && r.left <= x + 4 && r.top >= y - 4 && r.top <= y + 4 &&
            r.right - r.left == w && r.bottom - r.top == h,
            "got %ld,%ld %ldx%ld (expect %d,%d %dx%d)",
            (long)r.left, (long)r.top, (long)(r.right - r.left),
            (long)(r.bottom - r.top), x, y, w, h);
    return TRUE;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    WINDOWPLACEMENT placement;
    int round;

    t_begin("winehua_t_win_minimize", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_WinMin";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    hwnd = CreateWindowExA(0, "WineHuaT_WinMin", "minmax", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           120, 90, 500, 400, NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "hwnd=%p", hwnd);
    if (!hwnd)
        return t_finish();
    check_rect(hwnd, 120, 90, 500, 400, "initial-rect");

    for (round = 1; round <= 3; ++round)
    {
        TEXPR(ShowWindow(hwnd, SW_MINIMIZE));
        t_check("minimized-iconic", IsIconic(hwnd) != 0, "round %d", round);
        /* 最小化语义：上游把窗口移到 -32000 哨兵位置 */
        {
            RECT r;
            TEXPR(GetWindowRect(hwnd, &r));
            t_check("minimized-sentinel", r.left == -32000 && r.top == -32000,
                    "round %d got %ld,%ld", round, (long)r.left, (long)r.top);
        }
        TEXPR(ShowWindow(hwnd, SW_RESTORE));
        t_check("restored-normal", !IsIconic(hwnd) && IsWindowVisible(hwnd),
                "round %d", round);
        check_rect(hwnd, 120, 90, 500, 400, round == 3 ? "restored-rect-final" : "restored-rect");
    }

    /* placement 查询路径 */
    placement.length = sizeof(placement);
    TEXPR(GetWindowPlacement(hwnd, &placement));
    t_check("placement-normal", placement.showCmd == SW_SHOWNORMAL,
            "showCmd=0x%X", placement.showCmd);

    DestroyWindow(hwnd);
    return t_finish();
}
