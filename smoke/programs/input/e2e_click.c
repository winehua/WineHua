/* winehua_t_e2e_click — 点击命中（P1，手段 I+R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.15。
 * 失败特征：系统性偏移 = letterbox/fit 逆映射断；单象限错 = hit-test 断。
 * 协议：开窗画四色象限 → 注入四象限中心各一次 → 断言四次 WM_LBUTTONDOWN
 * 的客户区坐标落在对应象限（±3px）。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 320
#define CLIENT_H 240
#define ORIGIN_X 60
#define ORIGIN_Y 50

/* 象限中心（客户区坐标）与期望象限判定 */
static const POINT g_centers[4] = {
    {80, 60}, {240, 60}, {80, 180}, {240, 180},
};

static POINT g_hits[4]; /* 客户区坐标（lparam） */
static int g_n_hits;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_LBUTTONDOWN && g_n_hits < 4)
    {
        g_hits[g_n_hits].x = (short)LOWORD(lparam);
        g_hits[g_n_hits].y = (short)HIWORD(lparam);
        g_n_hits++;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
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
    HWND hwnd;
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    int i, wait;

    t_begin("winehua_t_e2e_click", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_E2EClick";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExA(0, "WineHuaT_E2EClick", "click",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    UpdateWindow(hwnd);
    /* 四色象限（视觉锚；命中判定走坐标不依赖读回） */
    {
        HDC dc = GetDC(hwnd);
        if (dc)
        {
            static const COLORREF colors[4] = {
                RGB(200, 30, 30), RGB(30, 200, 30),
                RGB(30, 30, 200), RGB(200, 200, 30),
            };
            for (i = 0; i < 4; ++i)
            {
                RECT r = {i % 2 * CLIENT_W / 2, i / 2 * CLIENT_H / 2,
                          (i % 2 + 1) * CLIENT_W / 2, (i / 2 + 1) * CLIENT_H / 2};
                HBRUSH b = CreateSolidBrush(colors[i]);
                FillRect(dc, &r, b);
                DeleteObject(b);
            }
            ReleaseDC(hwnd, dc);
        }
    }
    pump(200);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    {
        POINT origin = {0, 0};
        ClientToScreen(hwnd, &origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
        if (!f)
        {
            DestroyWindow(hwnd);
            return t_finish();
        }
        fprintf(f, "{\"actions\":[");
        for (i = 0; i < 4; ++i)
        {
            fprintf(f, "%s{\"type\":\"click\",\"x\":%d,\"y\":%d}", i ? "," : "",
                    origin.x + g_centers[i].x, origin.y + g_centers[i].y);
        }
        fprintf(f, "],\"actionGapMs\":350}");
        fclose(f);
        t_metric("client-origin", "%ld,%ld", (long)origin.x, (long)origin.y);
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
    pump(800);

    t_check("hit-count", g_n_hits == 4, "got %d expect 4", g_n_hits);
    for (i = 0; i < 4 && i < g_n_hits; ++i)
    {
        char name[32];
        int in_quadrant = (g_hits[i].x / (CLIENT_W / 2) == g_centers[i].x / (CLIENT_W / 2)) &&
                          (g_hits[i].y / (CLIENT_H / 2) == g_centers[i].y / (CLIENT_H / 2));
        int near_center = abs(g_hits[i].x - g_centers[i].x) <= 3 &&
                          abs(g_hits[i].y - g_centers[i].y) <= 3;
        snprintf(name, sizeof(name), "hit-q%d", i);
        t_check(name, in_quadrant && near_center,
                "got (%ld,%ld) expect (%ld,%ld) quadrant_ok=%d",
                (long)g_hits[i].x, (long)g_hits[i].y,
                (long)g_centers[i].x, (long)g_centers[i].y, in_quadrant);
    }

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
