// Manual device fixture. Compile with -DWINDOW_CYCLE for a bounded window-size
// cycle; without it, show a controlled modal owned by the real game window.
// The fixture never terminates the game or edits save files.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HWND game = FindWindowW(L"SDL_app", L"Darkest Dungeon");
    if (!game) return 2;
    FILE *out = _wfopen(L"C:\\windows\\temp\\winehua_game_window_regression.txt", L"wb");
    if (!out) return 3;
#ifdef WINDOW_CYCLE
    RECT original, small, restored;
    const LONG_PTR style = GetWindowLongPtrW(game, GWL_STYLE);
    GetWindowRect(game, &original);
    fprintf(out, "original=%ld,%ld,%ld,%ld style=%llx\n", original.left, original.top,
            original.right, original.bottom, (unsigned long long)style);
    SetWindowLongPtrW(game, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    SetWindowPos(game, HWND_TOP, 140, 110, 740, 550, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    GetWindowRect(game, &small);
    fprintf(out, "small=%ld,%ld,%ld,%ld\n", small.left, small.top, small.right, small.bottom);
    fflush(out);
    Sleep(10000);
    SetWindowLongPtrW(game, GWL_STYLE, style);
    SetWindowPos(game, HWND_TOP, original.left, original.top,
                 original.right - original.left, original.bottom - original.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    GetWindowRect(game, &restored);
    fprintf(out, "restored=%ld,%ld,%ld,%ld\n", restored.left, restored.top,
            restored.right, restored.bottom);
#else
    fputs("modal-open\n", out); fflush(out);
    const int result = MessageBoxW(game, L"Controlled game popup regression. Use the virtual Enter key to dismiss.",
        L"WineHua game popup regression", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    fprintf(out, "modal-closed result=%d game-alive=%d\n", result, IsWindow(game));
#endif
    fclose(out);
    return 0;
}
