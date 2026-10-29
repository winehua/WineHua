/* winehua_t_win_owned — owner 关系与模态禁用链（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.1。
 * 失败特征：owned 判成独立顶层 = 受管判据/私有 owner 链断（企业微信类回归）。
 */
#include "../common/winehua_t_check.h"

static LRESULT CALLBACK t_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND owner, owned, second;
    LONG ex_style;

    t_begin("winehua_t_win_owned", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = t_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_WinOwned";
    TCHECK("register-class", RegisterClassA(&wc) != 0);

    owner = CreateWindowExA(0, "WineHuaT_WinOwned", "owner",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 60, 480, 360,
                            NULL, NULL, wc.hInstance, NULL);
    t_check("create-owner", owner != NULL, "hwnd=%p", owner);
    if (!owner)
        return t_finish();

    /* owned popup：带 owner */
    owned = CreateWindowExA(WS_EX_DLGMODALFRAME, "WineHuaT_WinOwned", "owned",
                            WS_POPUP | WS_VISIBLE | WS_CAPTION, 140, 120, 320, 240,
                            owner, NULL, wc.hInstance, NULL);
    t_check("create-owned", owned != NULL, "hwnd=%p", owned);
    if (!owned)
    {
        DestroyWindow(owner);
        return t_finish();
    }

    TEXPR(GetWindow(owned, GW_OWNER) == owner);
    t_check("owner-backlink-none", GetWindow(owner, GW_OWNER) == NULL, "owner has no owner");

    ex_style = GetWindowLongA(owned, GWL_EXSTYLE);
    t_check("owned-exstyle-modal", (ex_style & WS_EX_DLGMODALFRAME) != 0,
            "exstyle=0x%08lX", ex_style);

    /* 模态禁用：owner 被 disable，owned 应保持可用。
     * EnableWindow 返回值 = 调用前窗口是否 disabled（MSDN）：之前 enabled
     * 的窗口被 disable 返回 0；re-enable 时返回非零。 */
    {
        BOOL enabled_before = IsWindowEnabled(owner);
        BOOL prev = EnableWindow(owner, FALSE);
        t_check("enable-owner-off", prev == 0 && enabled_before,
                "prev=%d enabled_before=%d err=%lu", prev, enabled_before,
                GetLastError());
    }
    t_check("owner-disabled", !IsWindowEnabled(owner), "after EnableWindow(FALSE)");
    t_check("owned-still-enabled", IsWindowEnabled(owned), "owned unaffected");
    TEXPR(EnableWindow(owner, TRUE));
    t_check("owner-re-enabled", IsWindowEnabled(owner), "after EnableWindow(TRUE)");

    /* 无 owner 的第二 popup 不应有 owner 关系 */
    second = CreateWindowExA(0, "WineHuaT_WinOwned", "second",
                             WS_POPUP | WS_VISIBLE | WS_CAPTION, 200, 180, 240, 160,
                             NULL, NULL, wc.hInstance, NULL);
    if (second)
    {
        t_check("second-no-owner", GetWindow(second, GW_OWNER) == NULL, "no owner");
        DestroyWindow(second);
    }

    DestroyWindow(owned);
    t_check("owned-destroyed", !IsWindow(owned), "owned gone");
    TEXPR(IsWindow(owner)); /* 销毁 owned 不应连带 owner */
    DestroyWindow(owner);
    return t_finish();
}
