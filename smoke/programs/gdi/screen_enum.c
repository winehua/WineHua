/* winehua_t_screen_enum — 显示器枚举（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.5。
 * 失败特征：几何不一致 = 多屏枚举/虚拟屏映射断。
 */
#include "../common/winehua_t_check.h"

static BOOL CALLBACK monitor_proc(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data)
{
    MONITORINFOEXA info;
    int *count = (int *)data;
    (void)monitor;
    (void)dc;

    (*count)++;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetMonitorInfoA(monitor, (MONITORINFO *)&info))
    {
        char label[64];
        snprintf(label, sizeof(label), "monitor-%d-rect", *count);
        t_check(label, (info.rcMonitor.right - info.rcMonitor.left) > 0 &&
                       (info.rcMonitor.bottom - info.rcMonitor.top) > 0,
                "%ldx%ld at %ld,%ld",
                (long)(info.rcMonitor.right - info.rcMonitor.left),
                (long)(info.rcMonitor.bottom - info.rcMonitor.top),
                (long)info.rcMonitor.left, (long)info.rcMonitor.top);
        if (info.dwFlags & MONITORINFOF_PRIMARY)
        {
            snprintf(label, sizeof(label), "primary-%d-geom", *count);
            t_check(label,
                    info.rcMonitor.right - info.rcMonitor.left == GetSystemMetrics(SM_CXSCREEN) &&
                    info.rcMonitor.bottom - info.rcMonitor.top == GetSystemMetrics(SM_CYSCREEN),
                    "monitor %ldx%ld vs SM %dx%d",
                    (long)(info.rcMonitor.right - info.rcMonitor.left),
                    (long)(info.rcMonitor.bottom - info.rcMonitor.top),
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        }
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    DEVMODEA dm;
    int monitors = 0;
    DWORD i = 0, found_current = 0;
    BOOL have_current;

    t_begin("winehua_t_screen_enum", argc, argv);

    EnumDisplayMonitors(NULL, NULL, monitor_proc, (LPARAM)&monitors);
    t_check("monitor-count", monitors >= 1, "got %d", monitors);

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    have_current = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    t_check("current-settings", have_current, "enum failed");
    if (have_current)
    {
        t_metric("current-mode", "%lux%lux%lu",
                 (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
                 (unsigned long)dm.dmBitsPerPel);
        /* 模式表含当前模式 */
        {
            DEVMODEA mode;
            memset(&mode, 0, sizeof(mode));
            mode.dmSize = sizeof(mode);
            while (EnumDisplaySettingsA(NULL, i, &mode))
            {
                if (mode.dmPelsWidth == dm.dmPelsWidth &&
                    mode.dmPelsHeight == dm.dmPelsHeight)
                {
                    found_current = 1;
                    break;
                }
                memset(&mode, 0, sizeof(mode));
                mode.dmSize = sizeof(mode);
                i++;
            }
            t_check("mode-table-contains-current", found_current,
                    "scanned %lu modes", i);
            t_metric("mode-table-size", "%lu", i + (found_current ? 0 : 1));
        }
    }
    t_metric("sm-screen", "%dx%d", GetSystemMetrics(SM_CXSCREEN),
             GetSystemMetrics(SM_CYSCREEN));
    return t_finish();
}
