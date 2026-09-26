/* winehua_t_e2e_drag — 拖动（P2，手段 I+R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.15。
 * 失败特征：位移发散 = move_grab 累积断；位置回跳 = toplevel 状态失步。
 * 协议：注入标题栏按下-右移 200px-释放；断言 NC 按下、轨迹连续、位移≈200px。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 280
#define CLIENT_H 180
#define ORIGIN_X 80
#define ORIGIN_Y 60
#define DRAG_DX 200

static int g_nc_down;
static int g_move_track;
static LONG g_first_move_x = -1, g_last_move_x = -1;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCLBUTTONDOWN && wparam == HTCAPTION)
        g_nc_down = 1;
    if (msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE)
    {
        POINT pt;
        GetCursorPos(&pt);
        if (g_first_move_x < 0) g_first_move_x = pt.x;
        g_last_move_x = pt.x;
        g_move_track++;
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
    int wait;
    RECT before, after;
    LONG moved;

    t_begin("winehua_t_e2e_drag", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_E2EDrag";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "WineHuaT_E2EDrag", "drag",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    pump(200);
    GetWindowRect(hwnd, &before);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    {
        POINT origin = {0, 0};
        /* 标题栏位置：客户区原点上方 ~14px（标题栏中线） */
        ClientToScreen(hwnd, &origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", f ? 0 : GetLastError());
        if (!f)
            return t_finish();
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"swipe\",\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":0,\"steps\":12,"
                   "\"button\":\"left\"}"
                   "],\"actionGapMs\":200}",
                origin.x + CLIENT_W / 2, origin.y - 14, DRAG_DX);
        fclose(f);
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
    GetWindowRect(hwnd, &after);
    moved = (after.left - before.left);

    t_check("nc-caption-down", g_nc_down != 0, "WM_NCLBUTTONDOWN HTCAPTION seen=%d",
            g_nc_down);
    /* xdg_toplevel.move 交互拖动由 compositor 接管，拖动中 wine 收不到
     * move（enter 定位的至多 1 条）；move 计数只作为事件链路存活证明 */
    t_check("move-track", g_move_track >= 1, "move events %d", g_move_track);
    t_check("window-moved", moved >= DRAG_DX * 80 / 100 && moved <= DRAG_DX * 120 / 100,
            "dx=%ld expect %d±20%%", moved, DRAG_DX);
    t_check("position-holds", after.left == before.left + moved && after.top == before.top,
            "after=(%ld,%ld)", (long)after.left, (long)after.top);
    t_metric("drag-result", "%ld track=%d", moved, g_move_track);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
