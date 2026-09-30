/* winehua_t_win_child — 子窗口嵌套与坐标映射（P2，手段 R+B）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 失败特征：嵌套层序错 = 客户端合成/surface 压平链断。
 */
#include "../common/winehua_t_check.h"

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static HWND make_child(HWND parent, const char *cls, const char *title,
                       int x, int y, int w, int h, COLORREF bg)
{
    HWND child = CreateWindowExA(0, cls, title,
                                 WS_CHILD | WS_VISIBLE | WS_BORDER,
                                 x, y, w, h, parent, NULL, GetModuleHandleA(NULL), NULL);
    if (child)
    {
        HDC dc = GetDC(child);
        if (dc)
        {
            RECT r = {0, 0, w, h};
            HBRUSH brush = CreateSolidBrush(bg);
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            ReleaseDC(child, dc);
        }
    }
    return child;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND parent, child1, child2;
    POINT pt;

    t_begin("winehua_t_win_child", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_WinChild";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    parent = CreateWindowExA(0, "WineHuaT_WinChild", "parent",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             80, 60, 520, 400, NULL, NULL, wc.hInstance, NULL);
    t_check("create-parent", parent != NULL, "err=%lu", parent ? 0 : GetLastError());
    if (!parent)
        return t_finish();

    child1 = make_child(parent, "WineHuaT_WinChild", "child1",
                        20, 20, 220, 160, RGB(180, 40, 40));
    child2 = make_child(child1, "WineHuaT_WinChild", "child2",
                        30, 30, 120, 80, RGB(40, 40, 180));
    t_check("create-children", child1 != NULL && child2 != NULL,
            "c1=%p c2=%p", child1, child2);
    if (!(child1 && child2))
    {
        DestroyWindow(parent);
        return t_finish();
    }
    UpdateWindow(parent);

    /* 父子关系链 */
    t_check("child1-parent", GetParent(child1) == parent, "parent=%p", GetParent(child1));
    t_check("child2-parent", GetParent(child2) == child1, "parent=%p", GetParent(child2));
    t_check("child2-ancestor", IsChild(parent, child2) && IsChild(parent, child1),
            "IsChild chain");

    /* 坐标往返零误差：child2 内一点 → screen → parent 客户区 → 回 screen */
    pt.x = 60;
    pt.y = 40; /* child2 客户区内 */
    {
        POINT screen, back, in_parent;
        screen = pt;
        ClientToScreen(child2, &screen);
        back = screen;
        ScreenToClient(child2, &back);
        t_check("roundtrip-zero", back.x == pt.x && back.y == pt.y,
                "back=(%ld,%ld) expect (%ld,%ld)", (long)back.x, (long)back.y,
                (long)pt.x, (long)pt.y);
        in_parent = screen;
        ScreenToClient(parent, &in_parent);
        /* child1 在 parent (20,20)（客户区），child2 在 child1 (30,30)+边框。
         * 边框宽度 SM_CXSIZEFRAME/SM_CYCAPTION 影响精确值，只断言落在
         * parent 客户区的合理范围（> child1 偏移，且非负）。 */
        t_check("map-to-parent", in_parent.x > 40 && in_parent.y > 40,
                "in_parent=(%ld,%ld)", (long)in_parent.x, (long)in_parent.y);
    }

    /* B 半段：各层图案自读回（客户端合成序） */
    {
        HDC dc = GetDC(child1);
        if (dc)
        {
            COLORREF c = GetPixel(dc, 10, 10);
            t_check("child1-fill", GetRValue(c) > 150 && GetGValue(c) < 90,
                    "got %06lX", (unsigned long)c);
            ReleaseDC(child1, dc);
        }
        dc = GetDC(child2);
        if (dc)
        {
            COLORREF c = GetPixel(dc, 5, 5);
            t_check("child2-fill", GetBValue(c) > 150 && GetRValue(c) < 90,
                    "got %06lX", (unsigned long)c);
            ReleaseDC(child2, dc);
        }
    }

    DestroyWindow(child2);
    DestroyWindow(child1);
    t_check("children-gone", !IsWindow(child2) && !IsWindow(child1), "destroyed");
    DestroyWindow(parent);
    return t_finish();
}
