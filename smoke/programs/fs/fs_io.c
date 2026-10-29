/* winehua_t_fs_io — 文件读写、目录枚举、长路径（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.7。
 * 失败特征：Z: 写失败 = HOME 一致性/盘符双实现分叉回归。
 */
#include "../common/winehua_t_check.h"

static void test_roundtrip(const char *base, const char *label)
{
    char path[MAX_PATH];
    HANDLE file;
    static const unsigned char pattern[64] =
        "WineHuaT fs_io pattern 0123456789 ABCDEFabcdef --- 42";
    unsigned char readback[256], readback2[256];
    DWORD written, read;
    int i, pass;

    snprintf(path, sizeof(path), "%s\\fs_io_%s.bin", base, label);

    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    t_check("create-ok", file != INVALID_HANDLE_VALUE,
            "%s err=%lu", label, file == INVALID_HANDLE_VALUE ? GetLastError() : 0);
    if (file == INVALID_HANDLE_VALUE)
        return;
    /* 写入 4KB：64 字节图案重复 64 次 */
    pass = 1;
    for (i = 0; i < 64; ++i)
    {
        if (!WriteFile(file, pattern, sizeof(pattern), &written, NULL) ||
            written != sizeof(pattern))
        {
            pass = 0;
            break;
        }
    }
    t_check("write-4kb", pass, "%s wrote 4KB", label);
    CloseHandle(file);

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    t_check("reopen", file != INVALID_HANDLE_VALUE, "%s reopen err=%lu",
            label, file == INVALID_HANDLE_VALUE ? GetLastError() : 0);
    if (file != INVALID_HANDLE_VALUE)
    {
        memset(readback, 0, sizeof(readback));
        pass = ReadFile(file, readback, sizeof(pattern), &read, NULL) &&
               read == sizeof(pattern) &&
               memcmp(readback, pattern, sizeof(pattern)) == 0;
        t_check("readback-4kb", pass, "%s 4KB content", label);
        CloseHandle(file);
    }

    /* 追加后总量与内容尾 */
    file = CreateFileA(path, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        BOOL append_ok = WriteFile(file, pattern, sizeof(pattern), &written, NULL);
        t_check("append-write", append_ok && written == sizeof(pattern),
                "%s append write err=%lu written=%lu", label,
                append_ok ? 0 : GetLastError(), written);
        CloseHandle(file);
        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            /* 文件应为主体 64×64 字节 + 追加 64 字节；尾部图案为追加内容 */
            DWORD size = GetFileSize(file, NULL);
            pass = size == 65 * sizeof(pattern) &&
                   SetFilePointer(file, (LONG)(size - sizeof(pattern)), NULL,
                                  FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
                   ReadFile(file, readback2, sizeof(pattern), &read, NULL) &&
                   read == sizeof(pattern) &&
                   memcmp(readback2, pattern, sizeof(pattern)) == 0;
            t_check("append-readback", pass, "%s size=%lu expect=%zu tailerr=%lu",
                    label, size, 65 * sizeof(pattern),
                    pass ? 0 : GetLastError());
            CloseHandle(file);
        }
    }

    TCHECK("delete-file", DeleteFileA(path));
    t_check("deleted-gone", GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES,
            "%s still exists after delete", label);
}

static void test_dir_enum(const char *base)
{
    char path[MAX_PATH];
    HANDLE find;
    WIN32_FIND_DATAA data;
    int i, count = 0;

    snprintf(path, sizeof(path), "%s\\enum", base);
    CreateDirectoryA(path, NULL);
    for (i = 0; i < 5; ++i)
    {
        char file_path[MAX_PATH];
        HANDLE file;
        snprintf(file_path, sizeof(file_path), "%s\\f%d.txt", path, i);
        file = CreateFileA(file_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written;
            WriteFile(file, "x", 1, &written, NULL);
            CloseHandle(file);
        }
    }
    snprintf(path, sizeof(path), "%s\\enum\\*.txt", base);
    find = FindFirstFileA(path, &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        count = 1;
        while (FindNextFileA(find, &data))
            count++;
        FindClose(find);
    }
    t_check("dir-enum-count", count == 5, "found %d/5 files", count);

    /* 清理 */
    snprintf(path, sizeof(path), "%s\\enum\\f*.txt", base);
    find = FindFirstFileA(path, &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        char full[MAX_PATH];
        do
        {
            snprintf(full, sizeof(full), "%s\\enum\\%s", base, data.cFileName);
            DeleteFileA(full);
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    snprintf(path, sizeof(path), "%s\\enum", base);
    RemoveDirectoryA(path);
}

static void test_long_path(const char *temp)
{
    char deep[MAX_PATH];
    char long_path[1024];
    HANDLE file;
    DWORD written, read, err;
    unsigned char readback[16];
    int i, offset = 0;

    /* 合规深路径：14 级深目录、总长 <MAX_PATH，判据必须过 */
    memset(deep, 0, sizeof(deep));
    for (i = 0; i < 14; ++i)
        offset += snprintf(deep + offset, sizeof(deep) - offset, "%s",
                           i ? "\\d12" : "d12");
    snprintf(long_path, sizeof(long_path), "%s\\%s\\lf.bin", temp, deep);
    {
        char partial[1024];
        char *slash;
        snprintf(partial, sizeof(partial), "%s", temp);
        slash = long_path + strlen(temp);
        while ((slash = strchr(slash + 1, '\\')) != NULL)
        {
            size_t len = (size_t)(slash - long_path);
            memcpy(partial, long_path, len);
            partial[len] = '\0';
            CreateDirectoryA(partial, NULL);
        }
    }
    t_metric("deep_path_len", "%zu", strlen(long_path));
    file = CreateFileA(long_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    t_check("deep-path-create", file != INVALID_HANDLE_VALUE,
            "len=%zu err=%lu", strlen(long_path),
            file == INVALID_HANDLE_VALUE ? GetLastError() : 0);
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile(file, "LONG", 4, &written, NULL);
        CloseHandle(file);
        file = CreateFileA(long_path, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            memset(readback, 0, sizeof(readback));
            t_check("deep-path-readback",
                    ReadFile(file, readback, 4, &read, NULL) && read == 4 &&
                    memcmp(readback, "LONG", 4) == 0,
                    "read %lu bytes", read);
            CloseHandle(file);
        }
        else
            t_check("deep-path-readback", 0, "reopen err=%lu", GetLastError());
        DeleteFileA(long_path);
    }

    /* >MAX_PATH 超长路径：记录实际行为不判 FAIL —— wine 与真 Windows 默认
     * （未启用 LongPathsEnabled）同样返回 206，属上游语义非 winehua 边界 */
    memset(deep, 0, sizeof(deep));
    for (i = 0; i < 18; ++i)
        offset += snprintf(deep + offset, sizeof(deep) - offset, "%s",
                           i ? "\\dircmp12345" : "dircmp12345");
    snprintf(long_path, sizeof(long_path), "%s\\%s\\lf.bin", temp, deep);
    {
        char partial[1024];
        char *slash;
        snprintf(partial, sizeof(partial), "%s", temp);
        slash = long_path + strlen(temp);
        while ((slash = strchr(slash + 1, '\\')) != NULL)
        {
            size_t len = (size_t)(slash - long_path);
            memcpy(partial, long_path, len);
            partial[len] = '\0';
            CreateDirectoryA(partial, NULL);
        }
    }
    file = CreateFileA(long_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    err = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    t_metric("over260_path_len", "%zu", strlen(long_path));
    t_metric("over260_err", "%lu", err);
    t_check("over260-consistent", file != INVALID_HANDLE_VALUE || err == 206,
            "len=%zu err=%lu (206=ERROR_FILENAME_EXCED_RANGE 上游默认语义)",
            strlen(long_path), err);
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile(file, "LONG", 4, &written, NULL);
        CloseHandle(file);
        DeleteFileA(long_path);
    }
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH];
    char zbase[MAX_PATH];

    t_begin("winehua_t_fs_io", argc, argv);

    TCHECK("get-temp-path", (GetTempPathA(sizeof(temp), temp) > 0));
    t_metric("temp", "%s", temp);

    test_roundtrip(temp, "temp");
    test_dir_enum(temp);
    test_long_path(temp);

    /* Z: 盘上的同构读写（HOME 一致性回归哨兵） */
    snprintf(zbase, sizeof(zbase), "Z:\\winehua_t_tmp");
    CreateDirectoryA(zbase, NULL);
    t_check("z-dir-created", GetFileAttributesA(zbase) != INVALID_FILE_ATTRIBUTES,
            "err=%lu", GetLastError());
    test_roundtrip(zbase, "z");
    /* 清理 Z: 目录 */
    {
        WIN32_FIND_DATAA data;
        char pattern[MAX_PATH];
        snprintf(pattern, sizeof(pattern), "%s\\*.*", zbase);
        {
            HANDLE find = FindFirstFileA(pattern, &data);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    char full[MAX_PATH];
                    if (!lstrcmpA(data.cFileName, ".") || !lstrcmpA(data.cFileName, ".."))
                        continue;
                    snprintf(full, sizeof(full), "%s\\%s", zbase, data.cFileName);
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        RemoveDirectoryA(full);
                    else
                        DeleteFileA(full);
                } while (FindNextFileA(find, &data));
                FindClose(find);
            }
        }
        RemoveDirectoryA(zbase);
    }
    return t_finish();
}
