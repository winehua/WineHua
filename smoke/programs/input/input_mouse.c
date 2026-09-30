/* winehua_t_input_mouse — 鼠标注入配对（P1，手段 I，注入设施首例）。
 * 判定规格见 docs/engineering/testing-programs.md §3.3。
 * 失败特征：坐标系统性偏移 = letterbox 逆映射断；丢失 = CLICK-PIPE/队列断。
 * 协议：--automation 开窗 → 写 inject-request（声明注入序列，guest 桌面坐标）
 * → 等 inject-done → 断言收到的消息计数与坐标 → result。
 * 双击判定要求窗口类 CS_DBLCLKS；同一位置二次 press 在双击窗口内 →
 * WM_LBUTTONDBLCLK 替代第三次 WM_LBUTTONDOWN。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 320
#define CLIENT_H 240
#define ORIGIN_X 60
#define ORIGIN_Y 50

/* 注入声明（与 SmokeRunner 的注入编排同源：guest 桌面坐标） */
static const struct { const char *type; int x, y, right; } g_actions[] = {
    { "click",       100, 100, 0 },
    { "click",       200, 150, 0 },
    { "click",       150, 200, 1 },
    { "doubleClick", 120, 120, 0 },
};
#define N_ACTIONS (sizeof(g_actions) / sizeof(g_actions[0]))

struct hit { int down, dbl, rdown, up; POINT pt; };
static struct hit g_left[8];
static struct hit g_right[4];
static int g_n_left, g_n_right;
static int g_up_left, g_up_right;
static POINT g_client_origin; /* 客户区原点的 guest 桌面坐标（声明与断言同源） */

static void record(int msg, struct hit *slot)
{
    DWORD pos = GetMessagePos();
    slot->pt.x = (short)LOWORD(pos);
    slot->pt.y = (short)HIWORD(pos);
    (void)msg;
}

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        if (g_n_left < 8) { g_left[g_n_left].down = 1; record(msg, &g_left[g_n_left]); g_n_left++; }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (g_n_left > 0 && g_n_left < 8)
        { g_left[g_n_left - 1].dbl = 1; record(msg, &g_left[g_n_left - 1]); g_n_left++; }
        return 0;
    case WM_LBUTTONUP:
        g_up_left++;
        return 0;
    case WM_RBUTTONDOWN:
        if (g_n_right < 4) { g_right[g_n_right].down = 1; record(msg, &g_right[g_n_right]); g_n_right++; }
        return 0;
    case WM_RBUTTONUP:
        g_up_right++;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static BOOL near_point(const POINT *got, int x, int y)
{
    return got->x >= x - 2 && got->x <= x + 2 && got->y >= y - 2 && got->y <= y + 2;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    RECT frame = {0, 0, CLIENT_W, CLIENT_H};
    HWND hwnd;
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    int i, wait;

    t_begin("winehua_t_input_mouse", argc, argv);

    /* 协作文件名按运行时 testId（带架构后缀）拼，与 runner 的编排对齐 */
    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_InputMouse";
    wc.style = CS_DBLCLKS; /* 双击消息的前提 */
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExA(0, "WineHuaT_InputMouse", "mouse",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    UpdateWindow(hwnd);
    Sleep(200);

    /* 清上轮残留（request/done 不清会让 done 判定提前通过） */
    DeleteFileA(req_path);
    DeleteFileA(done_path);

    /* 写注入声明：动作目标点 = 客户区原点（guest 桌面坐标）+ 相对点。
     * origin 同时作为断言基准 —— 声明与断言必须同源，否则差量正好是
     * 边框+标题栏（首版用窗口原点断言，实测系统性偏移 (+3,+22)）。 */
    {
        ClientToScreen(hwnd, &g_client_origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
        if (!f)
        {
            DestroyWindow(hwnd);
            return t_finish();
        }
        fprintf(f, "{\"actions\":[");
        for (i = 0; i < (int)N_ACTIONS; ++i)
        {
            fprintf(f, "%s{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\"}",
                    i ? "," : "", g_actions[i].type,
                    g_client_origin.x + g_actions[i].x, g_client_origin.y + g_actions[i].y,
                    g_actions[i].right ? "right" : "left");
        }
        fprintf(f, "],\"actionGapMs\":300}");
        fclose(f);
        t_metric("client-origin", "%ld,%ld", (long)g_client_origin.x, (long)g_client_origin.y);
    }

    /* 等 runner 注入完成（done 文件出现；runner 失败也会写 error 终态） */
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
        {
            /* 消息泵：保持 wndproc 收消息 */
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }
        Sleep(100);
    }
    t_check("inject-done-seen", wait < 300, "waited %d00ms", wait);

    /* done 写出时最后的 release 可能还在管道上（native delay-release），留足缓冲 */
    {
        MSG msg;
        for (i = 0; i < 16; ++i)
        {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            Sleep(50);
        }
    }

    /* 断言：计数 + 坐标（期望与声明同源 = 客户区原点 + 相对点）。
     * 左 press 序列 = A/B 各一次 + doubleClick 首次 press（记 WM_LBUTTONDOWN），
     * doubleClick 第二次 press 记 WM_LBUTTONDBLCLK 并把标志写回同一槽 ——
     * 故 g_n_left = 3 次 DOWN + 1 次 DBLCLK = 4。 */
    t_check("left-count", g_n_left == 4, "got %d expect 4 (A,B + dbl first press"
            " + dblclk slot)", g_n_left);
    t_check("dblclk-received", g_left[2].down && g_left[2].dbl,
            "dbl slot down=%d dbl=%d", g_n_left > 2 ? g_left[2].down : 0,
            g_n_left > 2 ? g_left[2].dbl : 0);
    t_check("right-count", g_n_right == 1, "got %d expect 1", g_n_right);
    t_check("up-counts", g_up_left == 4 && g_up_right == 1,
            "left up=%d (expect 4) right up=%d (expect 1)", g_up_left, g_up_right);

    if (g_n_left >= 1)
        t_check("hit-A", near_point(&g_left[0].pt,
                g_client_origin.x + g_actions[0].x, g_client_origin.y + g_actions[0].y),
                "got %ld,%ld expect %d,%d", (long)g_left[0].pt.x, (long)g_left[0].pt.y,
                g_client_origin.x + g_actions[0].x, g_client_origin.y + g_actions[0].y);
    if (g_n_left >= 2)
        t_check("hit-B", near_point(&g_left[1].pt,
                g_client_origin.x + g_actions[1].x, g_client_origin.y + g_actions[1].y),
                "got %ld,%ld expect %d,%d", (long)g_left[1].pt.x, (long)g_left[1].pt.y,
                g_client_origin.x + g_actions[1].x, g_client_origin.y + g_actions[1].y);
    if (g_n_left >= 3)
        t_check("hit-D", near_point(&g_left[2].pt,
                g_client_origin.x + g_actions[3].x, g_client_origin.y + g_actions[3].y),
                "got %ld,%ld expect %d,%d", (long)g_left[2].pt.x, (long)g_left[2].pt.y,
                g_client_origin.x + g_actions[3].x, g_client_origin.y + g_actions[3].y);
    if (g_n_right >= 1)
        t_check("hit-C-right", near_point(&g_right[0].pt,
                g_client_origin.x + g_actions[2].x, g_client_origin.y + g_actions[2].y),
                "got %ld,%ld expect %d,%d", (long)g_right[0].pt.x, (long)g_right[0].pt.y,
                g_client_origin.x + g_actions[2].x, g_client_origin.y + g_actions[2].y);

    t_metric("left", "%d", g_n_left);
    t_metric("right", "%d", g_n_right);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
