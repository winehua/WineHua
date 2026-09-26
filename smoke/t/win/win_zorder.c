/* winehua_t_win_zorder — Z 序操作（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 失败特征：客户侧序对而宿主序错 = zorder_policy/私有 Z 序链断。
 * D 半段（宿主协议摘要比对）依赖宿主摘要通道，未建前以 metric 记录不断言。
 */
#include "../common/winehua_t_check.h"

static HWND wnd_a, wnd_b, wnd_c;

/* 从 Z 序顶往下走链，收集 A/B/C 三个窗口的相对顺序（过滤无关窗口）。
 * 输出形如 "ABC"，长度即匹配到的窗口数。 */
static void collect_order(char *out, int size)
{
    HWND cur = GetTopWindow(NULL);
    int n = 0;
    out[0] = '\0';
    while (cur && n < size - 1)
    {
        if (cur == wnd_a) out[n++] = 'A';
        else if (cur == wnd_b) out[n++] = 'B';
        else if (cur == wnd_c) out[n++] = 'C';
        cur = GetWindow(cur, GW_HWNDNEXT);
    }
    out[n] = '\0';
}

static void check_order(const char *name, const char *expect)
{
    char order[8];
    collect_order(order, sizeof(order));
    t_check(name, !lstrcmpA(order, expect), "got=%s expect=%s", order, expect);
}

int main(int argc, char **argv)
{
    char order[8];
    RECT rect_a;

    t_begin("winehua_t_win_zorder", argc, argv);

    /* 位置互相重叠，保证 Z 序唯一决定可见关系 */
    wnd_a = t_create_toplevel("Static", "WineHuaT_ZOrdA", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              0, 40, 40, 300, 200, "ZHUA");
    wnd_b = t_create_toplevel("Static", "WineHuaT_ZOrdB", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              0, 60, 60, 300, 200, "ZHUB");
    wnd_c = t_create_toplevel("Static", "WineHuaT_ZOrdC", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              0, 80, 80, 300, 200, "ZHUC");
    t_check("create-three", wnd_a && wnd_b && wnd_c, "a=%p b=%p c=%p",
            wnd_a, wnd_b, wnd_c);
    if (!(wnd_a && wnd_b && wnd_c))
        return t_finish();

    /* 后建在上：初始序 C B A */
    check_order("initial-order", "CBA");

    /* A 提到顶 */
    TEXPR(SetWindowPos(wnd_a, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
    check_order("bring-a-top", "ACB");

    /* C 插到 hWndInsertAfter=A 的上一格（Windows 语义：放在 A 之前/上方）*/
    TEXPR(SetWindowPos(wnd_c, wnd_a, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
    check_order("insert-c-above-a", "CAB");
    /* A 的上方（GW_HWNDPREV）应是 C */
    t_check("a-prev-is-c", GetWindow(wnd_a, GW_HWNDPREV) == wnd_c,
            "prev=%p expect c=%p", GetWindow(wnd_a, GW_HWNDPREV), wnd_c);

    /* A 压到最底 */
    TEXPR(SetWindowPos(wnd_a, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
    check_order("send-a-bottom", "CBA");

    /* B 提顶：当前序 C B A，B 提顶后为 B C A */
    TEXPR(SetWindowPos(wnd_b, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
    check_order("bring-b-top", "BCA");

    collect_order(order, sizeof(order));
    t_metric("final-order", "%s", order);
    GetWindowRect(wnd_a, &rect_a);
    t_metric("a-rect", "%ldx%ld", (long)(rect_a.right - rect_a.left),
             (long)(rect_a.bottom - rect_a.top));

    DestroyWindow(wnd_a);
    DestroyWindow(wnd_b);
    DestroyWindow(wnd_c);
    return t_finish();
}
