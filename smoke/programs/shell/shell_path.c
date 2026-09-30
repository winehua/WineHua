/* winehua_t_shell_path — 系统路径映射（P1）。
 * 判定规格见 docs/engineering/testing-programs.md §3.18。
 * 失败特征：路径空/不可写 = shell 文件夹 → OHOS 目录映射断
 * （wineboot/profile 定制回归哨兵）。
 */
#include "../common/winehua_t_check.h"
#include <shlobj.h>

/* 断言名加 -writable 后缀，区分同目录的「存在」与「可写」两条 */
static void check_writable(const char *base, int pass, const char *file)
{
    char name[96];
    snprintf(name, sizeof(name), "%s-writable", base);
    t_check(name, pass, "file=%s err=%lu", file, pass ? 0 : GetLastError());
}

/* verify_writable=0 只断言路径非空（系统目录可能只读），=1 时真实建删文件 */
static void check_folder(int csidl, const char *label, int verify_writable)
{
    char path[MAX_PATH];
    HRESULT hr;

    memset(path, 0, sizeof(path));
    hr = SHGetFolderPathA(NULL, csidl, NULL, 0, path);
    t_check(label, SUCCEEDED(hr) && path[0], "hr=0x%08lX path=%s",
            (unsigned long)hr, path);
    if (SUCCEEDED(hr) && path[0])
    {
        t_metric(label, "%s", path);
        if (verify_writable)
        {
            char file[MAX_PATH + 32];
            HANDLE handle;
            DWORD written = 0;
            snprintf(file, sizeof(file), "%s\\winehua_t_write_test.tmp", path);
            handle = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
            if (handle != INVALID_HANDLE_VALUE)
            {
                WriteFile(handle, "ok", 2, &written, NULL);
                CloseHandle(handle);
            }
            check_writable(label, handle != INVALID_HANDLE_VALUE && written == 2, file);
            DeleteFileA(file);
        }
    }
}

int main(int argc, char **argv)
{
    t_begin("winehua_t_shell_path", argc, argv);

    /* 存在即可：系统目录不做写验证（可能只读） */
    check_folder(CSIDL_WINDOWS, "windows-dir", 0);
    check_folder(CSIDL_SYSTEM, "system-dir", 0);
    check_folder(CSIDL_FONTS, "fonts-dir", 0);
    check_folder(CSIDL_PROGRAM_FILES, "program-files", 0);

    /* 可写集合：用户数据目录必须真实可写 */
    check_folder(CSIDL_APPDATA, "appdata", 1);
    check_folder(CSIDL_LOCAL_APPDATA, "local-appdata", 1);
    check_folder(CSIDL_DESKTOPDIRECTORY, "desktop-dir", 1);
    check_folder(CSIDL_PERSONAL, "personal", 1);

    /* PROFILE 指向用户目录（HOME 映射回归）：PERSONAL 应位于 PROFILE 之下 */
    {
        char profile[MAX_PATH], personal[MAX_PATH];
        memset(profile, 0, sizeof(profile));
        memset(personal, 0, sizeof(personal));
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile)) &&
            SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, personal)))
        {
            t_check("profile-contains-personal",
                    !strncasecmp(personal, profile, lstrlenA(profile)),
                    "profile=%s personal=%s", profile, personal);
        }
    }
    return t_finish();
}
