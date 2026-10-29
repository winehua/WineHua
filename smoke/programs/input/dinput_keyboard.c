/* winehua_t_dinput_keyboard — DirectInput 键盘状态语义（P4，手段 I+R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * 失败特征：全 0=设备创建/协作级别断；位错=DIK 码与 evdev 换算链断。
 * 协议：DirectInput8Create→c_dfDIKeyboard→Acquire→host 注入 A/1 键序→
 * GetDeviceState(256B) 轮询状态位与边沿。
 */
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include "../common/winehua_t_check.h"
#include <dinput.h>

#define CLIENT_W 300
#define CLIENT_H 200
#define ORIGIN_X 60
#define ORIGIN_Y 50

static HMODULE g_dinput8;
static LPDIRECTINPUT8A g_di;
static LPDIRECTINPUTDEVICE8A g_kbd;
static HWND g_hwnd;
static volatile LONG g_quit;
static unsigned char g_keys[256];
static int g_a_down_seen, g_one_down_seen, g_ghost;
static DWORD g_polls;

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

/* 轮询窗口：记录 A/1 的按下边沿与幻影键 */
static void poll_loop(int ms)
{
    DWORD until = GetTickCount() + (DWORD)ms;
    while (GetTickCount() < until)
    {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (g_kbd &&
            g_kbd->lpVtbl->GetDeviceState(g_kbd, sizeof(g_keys), g_keys) == DI_OK)
        {
            int i;
            g_polls++;
            if (g_keys[DIK_A] & 0x80) g_a_down_seen = 1;
            if (g_keys[DIK_1] & 0x80) g_one_down_seen = 1;
            for (i = 0; i < 256; ++i)
                if ((g_keys[i] & 0x80) && i != DIK_A && i != DIK_1)
                    g_ghost++;
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
    int wait;

    t_begin("winehua_t_dinput_keyboard", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    hinst = GetModuleHandleA(NULL);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hinst;
    wc.lpszClassName = "WineHuaT_DIKbd";
    TCHECK("register-class", RegisterClassA(&wc) != 0);
    g_hwnd = CreateWindowExA(0, "WineHuaT_DIKbd", "dikbd",
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
           g_di->lpVtbl->CreateDevice(g_di, &GUID_SysKeyboard, &g_kbd, NULL) == DI_OK
           && g_kbd);
    if (!g_kbd)
        return t_finish();
    TCHECK("data-format",
           g_kbd->lpVtbl->SetDataFormat(g_kbd, &c_dfDIKeyboard) == DI_OK);
    TCHECK("cooperative-level",
           g_kbd->lpVtbl->SetCooperativeLevel(
               g_kbd, g_hwnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
    TCHECK("acquire", g_kbd->lpVtbl->Acquire(g_kbd) == DI_OK);
    pump(150);

    /* 写注入请求：click 建焦点 + A、1 键序（各一次按下/抬起） */
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    {
        POINT origin = {0, 0};
        ClientToScreen(g_hwnd, &origin);
        f = fopen(req_path, "w");
        t_check("write-request", f != NULL, "err=%lu", GetLastError());
        if (!f)
            return t_finish();
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"click\",\"x\":%d,\"y\":%d},"
                   "{\"type\":\"key\",\"code\":30},"
                   "{\"type\":\"key\",\"code\":2}"
                   "],\"actionGapMs\":250}",
                origin.x + CLIENT_W / 2, origin.y + CLIENT_H / 2);
        fclose(f);
    }

    poll_loop(15000); /* 注入期间持续轮询状态位 */
    t_check("inject-done-seen", 1, "polls=%lu", g_polls);

    t_check("state-dik-a", g_a_down_seen == 1, "a_down=%d polls=%lu",
            g_a_down_seen, g_polls);
    t_check("state-dik-1", g_one_down_seen == 1, "one_down=%d", g_one_down_seen);
    t_check("no-ghost-keys", g_ghost == 0, "ghost_polls=%d", g_ghost);
    TCHECK("unacquire", g_kbd->lpVtbl->Unacquire(g_kbd) == DI_OK);
    t_metric("dikbd-result", "a=%d one=%d ghost=%d polls=%lu",
             g_a_down_seen, g_one_down_seen, g_ghost, g_polls);

    g_kbd->lpVtbl->Release(g_kbd);
    g_di->lpVtbl->Release(g_di);
    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyWindow(g_hwnd);
    return t_finish();
}
