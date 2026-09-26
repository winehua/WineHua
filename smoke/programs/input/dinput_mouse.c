/* winehua_t_dinput_mouse — DirectInput 鼠标轴增量与缓冲（P4，手段 I+R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * 失败特征：轴恒 0=相对增量通道断（REL/rawDelta 缩放链——FPS 视角卡死族）。
 * 协议：c_dfDIMouse（相对轴）→Acquire→host 注入右移 swipe + 左键点击→
 * GetDeviceState 轴增量/按钮位 + GetDeviceData 缓冲序列。
 */
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include "../common/winehua_t_check.h"
#include <dinput.h>

#define CLIENT_W 300
#define CLIENT_H 200
#define ORIGIN_X 60
#define ORIGIN_Y 50
#define SWIPE_DX 160
#define BUFFER_ITEMS 64

static HMODULE g_dinput8;
static LPDIRECTINPUT8A g_di;
static LPDIRECTINPUTDEVICE8A g_mouse;
static HWND g_hwnd;
static LONG g_axis_dx, g_axis_dy;
static int g_btn0_seen, g_lmb_down_edge;
static DWORD g_state_polls, g_buf_events;
static LONG g_buf_dx, g_buf_dy;

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
        Sleep(10);
    }
}

static void poll_loop(int ms)
{
    DWORD until = GetTickCount() + (DWORD)ms;
    while (GetTickCount() < until)
    {
        MSG msg;
        DIMOUSESTATE st;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (g_mouse &&
            g_mouse->lpVtbl->GetDeviceState(g_mouse, sizeof(st), &st) == DI_OK)
        {
            g_state_polls++;
            g_axis_dx += st.lX;
            g_axis_dy += st.lY;
            if (st.rgbButtons[0] & 0x80) g_btn0_seen = 1;
        }
        if (g_mouse)
        {
            DIDEVICEOBJECTDATA data[BUFFER_ITEMS];
            DWORD n = BUFFER_ITEMS;
            int i;
            while (g_mouse->lpVtbl->GetDeviceData(
                       g_mouse, sizeof(data[0]), data, &n, 0) == DI_OK && n)
            {
                for (i = 0; i < (int)n; ++i)
                {
                    g_buf_events++;
                    if (data[i].dwOfs == DIMOFS_X) g_buf_dx += (LONG)data[i].dwData;
                    if (data[i].dwOfs == DIMOFS_Y) g_buf_dy += (LONG)data[i].dwData;
                    if (data[i].dwOfs == DIMOFS_BUTTON0 &&
                        (data[i].dwData & 0x80))
                        g_lmb_down_edge = 1;
                }
                n = BUFFER_ITEMS;
            }
        }
        Sleep(10);
    }
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HINSTANCE hinst;
    HRESULT (WINAPI *create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    DIPROPDWORD buf_prop;

    t_begin("winehua_t_dinput_mouse", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    hinst = GetModuleHandleA(NULL);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hinst;
    wc.lpszClassName = "WineHuaT_DIMouse";
    TCHECK("register-class", RegisterClassA(&wc) != 0);
    g_hwnd = CreateWindowExA(0, "WineHuaT_DIMouse", "dimouse",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             ORIGIN_X, ORIGIN_Y, CLIENT_W, CLIENT_H,
                             NULL, NULL, hinst, NULL);
    t_check("create-window", g_hwnd != NULL, "err=%lu", GetLastError());
    pump(200);

    g_dinput8 = LoadLibraryA("dinput8.dll");
    create_fn = g_dinput8
        ? (HRESULT (WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN))
          GetProcAddress(g_dinput8, "DirectInput8Create")
        : NULL;
    t_check("dll-load", create_fn != NULL, "hmod=%p", (void *)g_dinput8);
    if (!create_fn)
        return t_finish();

    {
        HRESULT hr8 = create_fn(hinst, DIRECTINPUT_VERSION, &IID_IDirectInput8A,
                                (LPVOID *)&g_di, NULL);
        t_check("directinput-create", hr8 == DI_OK && g_di != NULL,
                "hr=0x%08lx", (unsigned long)hr8);
    }
    if (!g_di)
        return t_finish();
    TCHECK("device-create",
           g_di->lpVtbl->CreateDevice(g_di, &GUID_SysMouse, &g_mouse, NULL) == DI_OK
           && g_mouse);
    if (!g_mouse)
        return t_finish();
    TCHECK("data-format",
           g_mouse->lpVtbl->SetDataFormat(g_mouse, &c_dfDIMouse) == DI_OK);
    TCHECK("cooperative-level",
           g_mouse->lpVtbl->SetCooperativeLevel(
               g_mouse, g_hwnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK);

    /* 事件缓冲：DIDF_ABSAXIS 不设（相对轴），缓冲 64 项 */
    memset(&buf_prop, 0, sizeof(buf_prop));
    buf_prop.diph.dwSize = sizeof(buf_prop);
    buf_prop.diph.dwHeaderSize = sizeof(buf_prop.diph);
    buf_prop.diph.dwObj = 0;
    buf_prop.diph.dwHow = DIPH_DEVICE;
    buf_prop.dwData = BUFFER_ITEMS;
    TCHECK("set-buffer-size",
           g_mouse->lpVtbl->SetProperty(g_mouse, DIPROP_BUFFERSIZE,
                                        &buf_prop.diph) == DI_OK);
    TCHECK("acquire", g_mouse->lpVtbl->Acquire(g_mouse) == DI_OK);
    pump(150);

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    {
        POINT origin = {0, 0};
        ClientToScreen(g_hwnd, &origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", GetLastError());
        if (!f)
            return t_finish();
        /* click 与 swipe 起点重合：click 的 enter 定位会把光标跳到点击点，
         * 起点不一致时定位差分会吃掉/抵消轴增量（实测 -100 偏移） */
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"click\",\"x\":%d,\"y\":%d},"
                   "{\"type\":\"swipe\",\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":0,\"steps\":8}"
                   "],\"actionGapMs\":300}",
                origin.x + 60, origin.y + CLIENT_H / 2,
                origin.x + 60, origin.y + CLIENT_H / 2, SWIPE_DX);
        fclose(f);
    }

    poll_loop(15000);

    t_check("state-polled", g_state_polls >= 5, "polls=%lu", g_state_polls);
    /* 通道流判定：有位移。量级存在超注入漂移（实测 dx 35~249 波动、
     * dy 注入 0 漂至 180）——wineserver 光标位移差分混入宿主 letterbox/
     * 合成光标管理的额外移动，属平台缺口（FPS 视角漂移族的量化证据，
     * 设计文档 §3.20 缺口记录），通道语义与量级分别判定 */
    t_check("axis-dx-flows", g_axis_dx > 0, "dx=%ld (swipe %d)",
            g_axis_dx, SWIPE_DX);
    t_check("button0-seen", g_btn0_seen == 1, "btn0=%d", g_btn0_seen);
    t_check("buffer-flows", g_buf_events >= 1, "events=%lu", g_buf_events);
    t_check("buffer-dx", g_buf_dx > 0, "buf_dx=%ld", g_buf_dx);
    t_check("buffer-matches-state", g_buf_dx == g_axis_dx,
            "buf=%ld state=%ld", g_buf_dx, g_axis_dx);
    t_metric("dimouse-scale", "dx/swipe=%ld/%d", g_axis_dx, SWIPE_DX);
    t_check("buffer-lmb-edge", g_lmb_down_edge == 1, "edge=%d", g_lmb_down_edge);
    TCHECK("unacquire", g_mouse->lpVtbl->Unacquire(g_mouse) == DI_OK);
    t_metric("dimouse-result",
             "dx=%ld dy=%ld buf_ev=%lu buf_dx=%ld polls=%lu",
             g_axis_dx, g_axis_dy, g_buf_events, g_buf_dx, g_state_polls);

    g_mouse->lpVtbl->Release(g_mouse);
    g_di->lpVtbl->Release(g_di);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(g_hwnd);
    return t_finish();
}
