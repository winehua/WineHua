/* winehua_t_reg_wow64 — WOW64 注册表视图（P5，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.8。
 * 失败特征：视图串扰=重定向断（32 位安装器写 HKLM 落错位置类）；
 * KEY_WOW64_64KEY 不可达=wow64 桥缺。
 * 用例自身在 32/64 位 guest 下都能执行：32 位进程的默认视图=32，64 位
 * 进程默认=64；显式 KEY_WOW64_*KEY 打开则与进程位数无关。判据写成
 * "两视图互不串扰 + 32 视图写入经 Wow6432Node 可见"，不预设进程位数。
 */
#include "../common/winehua_t_check.h"

#define TEST_KEY "Software\\WineHuaT\\Wow64View"

int main(int argc, char **argv)
{
    HKEY k64 = NULL, k32 = NULL, probe = NULL;
    LONG rc;
    DWORD disp, type, v64, v32, got, size;
    BOOL is_wow64 = FALSE;
    int is_wow64_proc;
    char value_name[128];

    t_begin("winehua_t_reg_wow64", argc, argv);

    IsWow64Process(GetCurrentProcess(), &is_wow64);
    is_wow64_proc = is_wow64 ? 1 : 0;
    t_check("iswow64-queried", 1, "is_wow64=%d", is_wow64_proc);
    t_metric("process-is-wow64", "%d", is_wow64_proc);

    rc = RegCreateKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
                         NULL, &k64, &disp);
    t_check("open-64-view", rc == ERROR_SUCCESS && k64, "rc=%ld", (long)rc);
    rc = RegCreateKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_32KEY,
                         NULL, &k32, &disp);
    t_check("open-32-view", rc == ERROR_SUCCESS && k32, "rc=%ld", (long)rc);

    if (k64 && k32)
    {
        v64 = 0x64000064;
        v32 = 0x32000032;

        rc = RegSetValueExA(k64, "ViewTag", 0, REG_DWORD,
                            (const BYTE *)&v64, sizeof(v64));
        rc |= RegSetValueExA(k32, "ViewTag", 0, REG_DWORD,
                             (const BYTE *)&v32, sizeof(v32));
        t_check("write-both-views", rc == ERROR_SUCCESS, "rc=%ld", (long)rc);

        /* 交叉读：64 视图必须见 0x64000064，32 视图见 0x32000032。
         * 通道用 RegOpenKeyEx + KEY_WOW64_*KEY（写路径同款，wine 已实现）；
         * RegGetValue 的 RRF_SUBKEY_WOW64*KEY 标志 wine 忽略（实测落到
         * 进程默认视图），不作判据通道。 */
        {
            HKEY r64 = NULL, r32 = NULL;
            rc = RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0,
                               KEY_READ | KEY_WOW64_64KEY, &r64);
            got = 0; size = sizeof(got); type = 0;
            if (rc == ERROR_SUCCESS)
                rc = RegGetValueA(r64, NULL, "ViewTag", RRF_RT_REG_DWORD,
                                  &type, &got, &size);
            t_check("read-back-64-view",
                    rc == ERROR_SUCCESS && got == v64,
                    "rc=%ld got=%08lx want=%08lx", (long)rc,
                    (unsigned long)got, (unsigned long)v64);
            if (r64) RegCloseKey(r64);

            rc = RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0,
                               KEY_READ | KEY_WOW64_32KEY, &r32);
            got = 0; size = sizeof(got); type = 0;
            if (rc == ERROR_SUCCESS)
                rc = RegGetValueA(r32, NULL, "ViewTag", RRF_RT_REG_DWORD,
                                  &type, &got, &size);
            t_check("read-back-32-view",
                    rc == ERROR_SUCCESS && got == v32,
                    "rc=%ld got=%08lx want=%08lx", (long)rc,
                    (unsigned long)got, (unsigned long)v32);
            if (r32) RegCloseKey(r32);
        }

        /* HKLM\Software 的 32 视图重定向：32 视图写 → Wow6432Node 可见 */
        {
            HKEY hlm32 = NULL, hlmprobe = NULL;
            DWORD hv = 0xFEEDF00D, hg = 0, hs = sizeof(hg);
            rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                 "Software\\WineHuaT\\Wow64Probe", 0, NULL,
                                 REG_OPTION_VOLATILE, KEY_WRITE | KEY_WOW64_32KEY,
                                 NULL, &hlm32, &disp);
            t_check("open-hklm-32-view", rc == ERROR_SUCCESS && hlm32,
                    "rc=%ld", (long)rc);
            if (hlm32)
            {
                RegSetValueExA(hlm32, "Probe", 0, REG_DWORD,
                               (const BYTE *)&hv, sizeof(hv));
                /* 64 视图下它应在 Wow6432Node 子树 */
                rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                                   "Software\\Wow6432Node\\WineHuaT\\Wow64Probe",
                                   0, KEY_READ | KEY_WOW64_64KEY, &hlmprobe);
                t_check("redirect-via-wow6432node", rc == ERROR_SUCCESS,
                        "rc=%ld", (long)rc);
                if (hlmprobe)
                {
                    hg = 0;
                    RegGetValueA(hlmprobe, NULL, "Probe", RRF_RT_REG_DWORD,
                                 &type, &hg, &hs);
                    t_check("redirect-value-matches", hg == hv,
                            "got=%08lx want=%08lx",
                            (unsigned long)hg, (unsigned long)hv);
                    RegDeleteValueA(hlmprobe, "Probe");
                    RegCloseKey(hlmprobe);
                }
                RegDeleteTreeA(hlm32, NULL);
                RegCloseKey(hlm32);
                RegDeleteKeyExA(HKEY_LOCAL_MACHINE,
                                "Software\\Wow6432Node\\WineHuaT\\Wow64Probe",
                                KEY_WOW64_64KEY, 0);
                RegDeleteKeyExA(HKEY_LOCAL_MACHINE,
                                "Software\\WineHuaT\\Wow64Probe",
                                KEY_WOW64_64KEY, 0);
            }
        }

        /* 清理：两视图各自删值删键 */
        wsprintfA(value_name, "cleanup");
        RegDeleteKeyValueA(HKEY_CURRENT_USER, TEST_KEY, "ViewTag");
        RegCloseKey(k64);
        RegCloseKey(k32);
        RegDeleteTreeA(HKEY_CURRENT_USER, TEST_KEY);
        /* 删除后两视图都不可见 */
        rc = RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0,
                           KEY_READ | KEY_WOW64_64KEY, &probe);
        t_check("cleanup-64-view-empty", rc == ERROR_FILE_NOT_FOUND,
                "rc=%ld", (long)rc);
        rc = RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0,
                           KEY_READ | KEY_WOW64_32KEY, &probe);
        t_check("cleanup-32-view-empty", rc == ERROR_FILE_NOT_FOUND,
                "rc=%ld", (long)rc);
    }

    return t_finish();
}
