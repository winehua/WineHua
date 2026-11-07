/* Controlled keyboard/IME/modal fixture. Does not access games or saves.
 * Build: x86_64-w64-mingw32-gcc -O2 -municode -mwindows this.c
 *        -o text_input_audit.exe -luser32 -lgdi32 -limm32
 * Evidence: C:\windows\temp\winehua_text_input_audit.txt (UTF-8).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include <stdio.h>

#define EDIT_ID 100
#define MODAL_ID 101
static FILE *audit;
static HWND edit;
static WNDPROC original_edit_proc;

static void log_text(const char *label, const WCHAR *text)
{
    char utf8[4096];
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL)) return;
    fprintf(audit, "%llu %s [%s]\n", (unsigned long long)GetTickCount64(), label, utf8);
    fflush(audit);
}

static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM key, LPARAM data)
{
    switch (message)
    {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_IME_KEYDOWN: case WM_IME_KEYUP:
            fprintf(audit, "%llu key message=%u vk=%u scan=%u extended=%u released=%u\n",
                    (unsigned long long)GetTickCount64(), message, (unsigned)key,
                    (unsigned)((data >> 16) & 0xff), (unsigned)((data >> 24) & 1),
                    (unsigned)((data >> 31) & 1));
            fflush(audit);
            break;
        case WM_CHAR:
            fprintf(audit, "%llu char U+%04X\n", (unsigned long long)GetTickCount64(), (unsigned)key);
            fflush(audit);
            break;
        case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_COMPOSITION:
            fprintf(audit, "%llu ime message=%u flags=0x%lx\n",
                    (unsigned long long)GetTickCount64(), message, (unsigned long)data);
            if (message == WM_IME_COMPOSITION)
            {
                HIMC context = ImmGetContext(window);
                WCHAR text[256];
                const DWORD kinds[] = {GCS_COMPSTR, GCS_RESULTSTR};
                for (unsigned i = 0; context && i < 2; ++i)
                {
                    if (!(data & kinds[i])) continue;
                    LONG bytes = ImmGetCompositionStringW(context, kinds[i], text, sizeof(text)-sizeof(WCHAR));
                    if (bytes >= 0 && bytes < (LONG)sizeof(text))
                    {
                        text[bytes / sizeof(WCHAR)] = 0;
                        log_text(i ? "ime-result" : "ime-preedit", text);
                    }
                }
                if (context) ImmReleaseContext(window, context);
            }
            fflush(audit);
            break;
    }
    return CallWindowProcW(original_edit_proc, window, message, key, data);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM key, LPARAM data)
{
    switch (message)
    {
        case WM_CREATE:
            CreateWindowExW(0, L"BUTTON", L"Open controlled modal; close with virtual Enter",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 16, 720, 32,
                    window, (HMENU)(INT_PTR)MODAL_ID, NULL, NULL);
            edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                    20, 64, 720, 240, window, (HMENU)(INT_PTR)EDIT_ID, NULL, NULL);
            SendMessageW(edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            original_edit_proc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
            SetFocus(edit);
            return 0;
        case WM_COMMAND:
            if (LOWORD(key) == EDIT_ID && HIWORD(key) == EN_CHANGE)
            {
                WCHAR text[1024];
                GetWindowTextW(edit, text, 1024);
                log_text("edit-text", text);
            }
            else if (LOWORD(key) == MODAL_ID && HIWORD(key) == BN_CLICKED)
            {
                fputs("modal-open\n", audit); fflush(audit);
                int result = MessageBoxW(window, L"Use virtual Enter to close this controlled dialog.",
                        L"WineHua text input modal", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
                fprintf(audit, "modal-closed result=%d\n", result); fflush(audit);
                SetFocus(edit);
            }
            return 0;
        case WM_DESTROY:
            fputs("closed\n", audit); fflush(audit);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, key, data);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    WNDCLASSW type = {0};
    WCHAR module[MAX_PATH];
    MSG message;
    (void)previous; (void)command; (void)show;
    audit = _wfopen(L"C:\\windows\\temp\\winehua_text_input_audit.txt", L"wb");
    if (!audit) return 2;
    if (GetModuleFileNameW(GetModuleHandleW(L"imm32.dll"), module, MAX_PATH)) log_text("imm32-module", module);
    type.hInstance = instance;
    type.lpfnWndProc = window_proc;
    type.lpszClassName = L"WineHuaTextInputAudit";
    type.hCursor = LoadCursorW(NULL, IDC_ARROW);
    type.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&type)) return 3;
    HWND window = CreateWindowExW(0, type.lpszClassName, L"WineHua keyboard and IME audit",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 90, 780, 370, NULL, NULL, instance, NULL);
    if (!window) return 4;
    SetForegroundWindow(window);
    SetFocus(edit);
    UpdateWindow(window);
    fputs("created\n", audit); fflush(audit);
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    fclose(audit);
    return 0;
}
