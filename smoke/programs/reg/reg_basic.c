/* winehua_t_reg_basic — 注册表三类值往返、重开持久性、枚举、删除（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.8。
 */
#include "../common/winehua_t_check.h"

#define KEY_PATH "Software\\WineHuaT\\reg_basic"

int main(int argc, char **argv)
{
    HKEY key, reopen;
    LONG rc;
    DWORD disp = 0, type = 0, size = 0, dword_value = 0, dword_expect = 0x57480042;
    char sz_read[128], binary_read[32];
    static const char *sz_expect = "WineHuaT 注册表值 42";
    static const BYTE binary_expect[8] = {0, 1, 2, 0xFE, 0xFF, 0x57, 0x48, 0x42};
    int subkeys = 0, subkey_expect = 2, i;

    t_begin("winehua_t_reg_basic", argc, argv);

    rc = RegCreateKeyExA(HKEY_CURRENT_USER, KEY_PATH, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, NULL, &key, &disp);
    t_check("create-key", rc == ERROR_SUCCESS, "rc=%ld", rc);
    if (rc != ERROR_SUCCESS)
        return t_finish();

    rc = RegSetValueExA(key, "TestSz", 0, REG_SZ, (const BYTE *)sz_expect, (DWORD)strlen(sz_expect) + 1);
    rc |= RegSetValueExA(key, "TestDword", 0, REG_DWORD, (const BYTE *)&dword_expect, sizeof(dword_expect));
    rc |= RegSetValueExA(key, "TestBin", 0, REG_BINARY, binary_expect, sizeof(binary_expect));
    t_check("set-values", rc == ERROR_SUCCESS, "rc=%ld", rc);
    RegCloseKey(key);

    /* 重开（走持久化文件而非句柄缓存）逐类读回 */
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_PATH, 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &reopen);
    t_check("reopen-key", rc == ERROR_SUCCESS, "rc=%ld", rc);
    if (rc != ERROR_SUCCESS)
        return t_finish();

    size = sizeof(sz_read);
    rc = RegQueryValueExA(reopen, "TestSz", NULL, &type, (BYTE *)sz_read, &size);
    t_check("query-sz", rc == ERROR_SUCCESS && type == REG_SZ &&
            lstrcmpA(sz_read, sz_expect) == 0,
            "rc=%ld type=%lu value='%s'", rc, type, sz_read);

    size = sizeof(dword_value);
    rc = RegQueryValueExA(reopen, "TestDword", NULL, &type, (BYTE *)&dword_value, &size);
    t_check("query-dword", rc == ERROR_SUCCESS && type == REG_DWORD &&
            dword_value == dword_expect,
            "rc=%ld got=0x%08X", rc, dword_value);

    size = sizeof(binary_read);
    rc = RegQueryValueExA(reopen, "TestBin", NULL, &type, (BYTE *)binary_read, &size);
    t_check("query-binary", rc == ERROR_SUCCESS && type == REG_BINARY &&
            size == sizeof(binary_expect) &&
            memcmp(binary_read, binary_expect, sizeof(binary_expect)) == 0,
            "rc=%ld size=%lu", rc, size);

    RegCloseKey(reopen);

    /* 建两个子键验证枚举 */
    rc = RegCreateKeyExA(HKEY_CURRENT_USER, KEY_PATH "\\Sub1", 0, NULL, 0, KEY_WRITE, NULL, &key, &disp);
    if (rc == ERROR_SUCCESS) RegCloseKey(key);
    rc |= RegCreateKeyExA(HKEY_CURRENT_USER, KEY_PATH "\\Sub2", 0, NULL, 0, KEY_WRITE, NULL, &key, &disp);
    if (rc == ERROR_SUCCESS) RegCloseKey(key);
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_PATH, 0, KEY_ENUMERATE_SUB_KEYS, &key);
    if (rc == ERROR_SUCCESS)
    {
        char name[64];
        DWORD name_len;
        i = 0;
        while ((rc = RegEnumKeyExA(key, i, name, (name_len = sizeof(name), &name_len), NULL, NULL, NULL, NULL)) == ERROR_SUCCESS)
        {
            subkeys++;
            i++;
        }
        RegCloseKey(key);
    }
    t_check("enum-subkeys", subkeys == subkey_expect, "found %d/%d", subkeys, subkey_expect);

    rc = RegDeleteTreeA(HKEY_CURRENT_USER, KEY_PATH);
    t_check("delete-tree", rc == ERROR_SUCCESS, "rc=%ld", rc);
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_PATH, 0, KEY_QUERY_VALUE, &key);
    t_check("deleted-gone", rc == ERROR_FILE_NOT_FOUND, "rc=%ld", rc);
    if (rc == ERROR_SUCCESS)
        RegCloseKey(key);
    return t_finish();
}
