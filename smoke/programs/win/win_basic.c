/* winehua_t_win_basic — 窗口创建、几何往返、标题、销毁（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 */
#include "../common/winehua_t_check.h"

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    RECT rect;
    char title[64];
    char classname[64];
    int x = 300, y = 200, w = 640, h = 480;

    t_begin("winehua_t_win_basic", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_WinBasic";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    hwnd = CreateWindowExA(0, "WineHuaT_WinBasic", "WineHuaT 窗口基础 42",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "hwnd=%p", hwnd);
    if (!hwnd)
        return t_finish();

    TCHECK("is-window", IsWindow(hwnd));
    TCHECK("is-visible", IsWindowVisible(hwnd));

    TCHECK("get-classname", GetClassNameA(hwnd, classname, sizeof(classname)) > 0 &&
           !lstrcmpA(classname, "WineHuaT_WinBasic"));

    TCHECK("get-title", GetWindowTextA(hwnd, title, sizeof(title)) > 0);
    t_check("title-roundtrip", lstrcmpA(title, "WineHuaT 窗口基础 42") == 0,
            "got '%s'", title);

    TCHECK("get-window-rect", GetWindowRect(hwnd, &rect));
    t_check("rect-width", rect.right - rect.left == w, "got %ld", (long)(rect.right - rect.left));
    t_check("rect-height", rect.bottom - rect.top == h, "got %ld", (long)(rect.bottom - rect.top));
    t_check("rect-x", rect.left >= x - 2 && rect.left <= x + 2, "got %ld", (long)rect.left);
    t_check("rect-y", rect.top >= y - 2 && rect.top <= y + 2, "got %ld", (long)rect.top);
    t_metric("rect", "%ld,%ld,%ld,%ld", (long)rect.left, (long)rect.top,
             (long)(rect.right - rect.left), (long)(rect.bottom - rect.top));

    /* 客户区应略小于窗口（有边框），但不为零 */
    TCHECK("get-client-rect", GetClientRect(hwnd, &rect));
    t_check("client-smaller-than-window", rect.right > 100 && rect.bottom > 100 &&
            (rect.right < w || rect.bottom < h),
            "client %ldx%ld", (long)rect.right, (long)rect.bottom);

    /* 移动到新位置再读回 */
    TCHECK("set-window-pos", SetWindowPos(hwnd, NULL, 50, 60, 800, 600,
                                          SWP_NOZORDER | SWP_NOACTIVATE));
    TCHECK("get-window-rect-2", GetWindowRect(hwnd, &rect));
    t_check("rect-after-move", rect.left >= 48 && rect.left <= 52 &&
            rect.top >= 58 && rect.top <= 62 &&
            rect.right - rect.left == 800 && rect.bottom - rect.top == 600,
            "got %ld,%ld %ldx%ld", (long)rect.left, (long)rect.top,
            (long)(rect.right - rect.left), (long)(rect.bottom - rect.top));

    DestroyWindow(hwnd);
    t_check("destroyed", !IsWindow(hwnd), "hwnd still valid after DestroyWindow");
    return t_finish();
}
