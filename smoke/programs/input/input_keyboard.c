/* winehua_t_input_keyboard — 键盘注入配对（P1，手段 I）。
 * 判定规格见 docs/engineering/testing-programs.md §3.3。
 * 失败特征：中文断 = IME/keymap 链；修饰态错 = 修饰键快照断。
 * 协议：开窗（caret 激活，IME 挂载条件）→ 点击窗口中心建立键盘焦点 →
 * 注入 "Aa1"（evdev 键序）+ ime "中"（commit 通道）→ 断言 KEYDOWN/CHAR。
 * WM_CHAR 期望：'A'(0x41,shift+a) 'a'(0x61) '1'(0x31) 0x4E2D。
 */
#include "../common/winehua_t_check.h"

#define CLIENT_W 320
#define CLIENT_H 240
#define ORIGIN_X 60
#define ORIGIN_Y 50

#define MAX_KEYS 16
struct keyev { UINT vk; int shift_down; };
static struct keyev g_downs[MAX_KEYS];
static int g_n_downs;
static WPARAM g_chars[MAX_KEYS];
static int g_n_chars;

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (g_n_downs < MAX_KEYS)
        {
            g_downs[g_n_downs].vk = (UINT)wparam;
            g_downs[g_n_downs].shift_down =
                (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
            g_n_downs++;
        }
        return 0;
    case WM_CHAR:
        if (g_n_chars < MAX_KEYS)
            g_chars[g_n_chars++] = wparam;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
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
    WNDCLASSW wc;
    RECT frame = {0, 0, CLIENT_W, CLIENT_H};
    HWND hwnd;
    char req_path[MAX_PATH], done_path[MAX_PATH];
    FILE *f;
    int i, wait;

    t_begin("winehua_t_input_keyboard", argc, argv);

    snprintf(req_path, sizeof(req_path),
             "C:\\smoke\\inject-request-%s.json", g_t.options.test_id);
    snprintf(done_path, sizeof(done_path),
             "C:\\smoke\\inject-done-%s.json", g_t.options.test_id);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = L"WineHuaT_InputKbd";
    TCHECK("register-class", RegisterClassW(&wc) != 0);

    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExW(0, L"WineHuaT_InputKbd", L"kbd",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX) | WS_VISIBLE,
                           ORIGIN_X, ORIGIN_Y,
                           frame.right - frame.left, frame.bottom - frame.top,
                           NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", hwnd != NULL, "err=%lu", hwnd ? 0 : GetLastError());
    if (!hwnd)
        return t_finish();
    UpdateWindow(hwnd);
    /* caret 非零 = IME 挂载条件（TextInput 光标矩形激活） */
    CreateCaret(hwnd, NULL, 2, 20);
    ShowCaret(hwnd);
    SetFocus(hwnd);
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
        /* 首动作 click 窗口中心：建立宿主侧键盘焦点与 lastInjectTl */
        fprintf(f, "{\"actions\":["
                   "{\"type\":\"click\",\"x\":%d,\"y\":%d},"
                   "{\"type\":\"type\",\"text\":\"Aa1\"},"
                   "{\"type\":\"ime\",\"text\":\"\\u4e2d\"}"
                   "],\"actionGapMs\":300}",
                origin.x + CLIENT_W / 2, origin.y + CLIENT_H / 2);
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
    pump(800); /* 尾部键序/commit 在管道上，留缓冲 */

    /* KEYDOWN：VK_A×2（'A' 与 'a'）+ VK_1×1；Shift 快照只有 'A' 那次置位。
     * IME commit 链可能产生额外 keydown（VK_PROCESSKEY 等），不计为失败，
     * 明细记 metric 供离线核对。 */
    {
        int vk_a = 0, vk_1 = 0, shift_hits = 0;
        char seq[128];
        int off = 0;
        for (i = 0; i < g_n_downs; ++i)
        {
            if (g_downs[i].vk == 'A') vk_a++;
            if (g_downs[i].vk == '1') vk_1++;
            if (g_downs[i].vk == 'A' && g_downs[i].shift_down) shift_hits++;
            if (off < (int)sizeof(seq) - 8)
                off += snprintf(seq + off, sizeof(seq) - off, "%02X%s",
                                g_downs[i].vk, g_downs[i].shift_down ? "+" : " ");
        }
        t_metric("keydowns", "%s", seq);
        t_check("vk-a-count", vk_a == 2, "got %d expect 2", vk_a);
        t_check("vk-1-count", vk_1 == 1, "got %d expect 1", vk_1);
        t_check("shift-snapshot", shift_hits == 1,
                "shift-down during KEYDOWN: %d (expect 1, only 'A')", shift_hits);
    }
    /* WM_CHAR 序列 'A','a','1','中' */
    t_check("char-count", g_n_chars == 4, "got %d expect 4", g_n_chars);
    if (g_n_chars == 4)
    {
        t_check("char-A", g_chars[0] == 'A', "got 0x%04lX", (unsigned long)g_chars[0]);
        t_check("char-a", g_chars[1] == 'a', "got 0x%04lX", (unsigned long)g_chars[1]);
        t_check("char-1", g_chars[2] == '1', "got 0x%04lX", (unsigned long)g_chars[2]);
        t_check("char-cjk", g_chars[3] == 0x4E2D, "got 0x%04lX expect 0x4E2D",
                (unsigned long)g_chars[3]);
        t_metric("chars", "%04lX,%04lX,%04lX,%04lX",
                 (unsigned long)g_chars[0], (unsigned long)g_chars[1],
                 (unsigned long)g_chars[2], (unsigned long)g_chars[3]);
    }
    else
    {
        char seq[128];
        int off = 0;
        for (i = 0; i < g_n_chars && off < (int)sizeof(seq) - 8; ++i)
            off += snprintf(seq + off, sizeof(seq) - off, "%04lX,",
                            (unsigned long)g_chars[i]);
        t_metric("chars", "%s", seq);
    }

    DeleteFileA(req_path);
    DeleteFileA(done_path);
    DestroyCaret();
    DestroyWindow(hwnd);
    return t_finish();
}
