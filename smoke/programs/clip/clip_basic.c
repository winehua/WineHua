/* winehua_t_clip_basic — 剪贴板同进程往返（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.6。
 * 先立 wine 内部剪贴板服务基线（与宿主 pasteboard 桥无关）；
 * 跨进程用例 clip_cross 在收敛项①落地后接入。
 */
#include "../common/winehua_t_check.h"

static const wchar_t PAYLOAD[] = L"WineHuaT 剪贴板往返 42 WineHuaT";

int main(int argc, char **argv)
{
    HANDLE data, readback;
    wchar_t *locked;
    UINT fmt;
    DWORD seq_before, seq_after;
    int fmt_seen = 0, roundtrip = 1;
    size_t bytes = (lstrlenW(PAYLOAD) + 1) * sizeof(wchar_t);

    t_begin("winehua_t_clip_basic", argc, argv);

    TCHECK("open-clipboard", OpenClipboard(NULL));
    seq_before = GetClipboardSequenceNumber();
    TCHECK("empty-clipboard", EmptyClipboard());

    data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    t_check("alloc", data != NULL, "err=%lu", data == NULL ? GetLastError() : 0);
    if (!data)
    {
        CloseClipboard();
        return t_finish();
    }
    locked = (wchar_t *)GlobalLock(data);
    if (locked)
    {
        memcpy(locked, PAYLOAD, bytes);
        GlobalUnlock(data);
    }
    t_check("set-data", locked != NULL && SetClipboardData(CF_UNICODETEXT, data) != NULL,
            "err=%lu", GetLastError());
    /* SetClipboardData 成功后 data 所有权归剪贴板，不再 GlobalFree */
    seq_after = GetClipboardSequenceNumber();
    t_check("sequence-advanced", seq_after > seq_before,
            "%lu -> %lu", seq_before, seq_after);
    TCHECK("close-clipboard", CloseClipboard());

    /* 重开读回 */
    TCHECK("open-clipboard", OpenClipboard(NULL));
    fmt = 0;
    while ((fmt = EnumClipboardFormats(fmt)) != 0)
    {
        if (fmt == CF_UNICODETEXT)
            fmt_seen = 1;
    }
    TCHECK("enum-formats", fmt_seen);

    readback = GetClipboardData(CF_UNICODETEXT);
    t_check("get-data", readback != NULL, "err=%lu", GetLastError());
    if (readback)
    {
        size_t size;
        const wchar_t *view = (const wchar_t *)GlobalLock(readback);
        if (view)
        {
            size = GlobalSize(readback);
            if (lstrlenW(view) != (int)lstrlenW(PAYLOAD) ||
                memcmp(view, PAYLOAD, bytes) != 0)
                roundtrip = 0;
            t_check("roundtrip", roundtrip, "got %d wchars (expect %d)",
                    lstrlenW(view), (int)lstrlenW(PAYLOAD));
            t_metric("payload_bytes", "%zu", size);
            GlobalUnlock(readback);
        }
        else
            t_check("roundtrip", 0, "GlobalLock failed err=%lu", GetLastError());
    }

    /* 清理：Empty 后关闭 */
    TCHECK("empty-clipboard", EmptyClipboard());
    TCHECK("close-clipboard", CloseClipboard());
    return t_finish();
}
