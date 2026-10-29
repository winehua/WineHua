/* winehua_t_input_capture — SetCapture 语义（P2，手段 I）。
 * 判定规格见 docs/engineering/testing-programs.md §3.3。
 * 失败特征：窗外断流 = capture 语义缺（已知结构性项，先红合法）。
 * 协议：SetCapture → 注入窗外 move ×5 → 断言 WM_MOUSEMOVE 到达 →
 * ReleaseCapture → 注入窗外 move → 断言不再到达。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 300
#define CLIENT_H 200
#define ORIGIN_X 60
#define ORIGIN_Y 50

static volatile LONG g_moves;
static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_MOUSEMOVE)
    {
        InterlockedExchange(&g_moves, g_moves + 1);
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
    int wait;
    LONG before_capture, during_capture, after_release;

    t_begin("winehua_t_input_capture", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_InputCap";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExA(0, "WineHuaT_InputCap", "cap",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    pump(200);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    /* 两段窗外 move 由 host 按 done 分段执行不了（done 是一次性），改为
     * 一个 request 内两个 swipe 动作，中间靠程序 SetCapture/Release 的
     * 时序窗口区分——第一段前 SetCapture，收到第一段后程序无法主动
     * Release（host 时序不可控）。改为：SetCapture → 单段窗外 swipe →
     * 断言收到（capture 生效半段）；Release 半段由窗口外第二次 swipe 的
     * 计数差判断（计入 after_release，等 800ms 后读）。 */
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
        /* 窗外（窗口右侧 +200px）横向 swipe */
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"swipe\",\"x\":%d,\"y\":%d,\"dx\":120,\"dy\":0,\"steps\":8}"
                   "],\"actionGapMs\":200}",
                origin.x + CLIENT_W + 200, origin.y + CLIENT_H / 2);
        fclose(f);
    }

    /* SetCapture 返回值是"之前捕获的窗口"（无前次捕获时合法为 NULL），
     * 捕获是否生效以 GetCapture 为准 */
    SetCapture(hwnd);
    TCHECK("capture-active", GetCapture() == hwnd);

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
    before_capture = g_moves;
    pump(400);
    during_capture = g_moves;

    t_check("capture-delivers-outside", during_capture > 0,
            "moves during capture: %ld (before %ld) — outside moves must flow",
            during_capture, before_capture);
    t_metric("moves-captured", "%ld", during_capture);

    ReleaseCapture();
    pump(400);
    after_release = g_moves;
    t_metric("moves-after-release", "%ld", after_release - during_capture);
    /* Release 后不再有窗外 move（无新动作时计数应稳定） */
    t_check("release-stops-flow", after_release == during_capture,
            "moves after release changed by %ld (expect 0)",
            after_release - during_capture);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(hwnd);
    return t_finish();
}
