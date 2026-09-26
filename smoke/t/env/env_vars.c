/* winehua_t_env_vars — 环境变量注入管线断言（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.8。
 * 失败特征：注入键缺失 = __env 通道/注入链断（env 管线首个可执行判据）。
 * 依赖：套件以 env 注入 WINEHUA_T_TEST_KEY=hello42（win32.json 已声明）。
 */
#include "../common/winehua_t_check.h"

int main(int argc, char **argv)
{
    char buffer[256];
    DWORD rc;
    LPSTR block, cursor;
    int count = 0, dup_found = 0;

    t_begin("winehua_t_env_vars", argc, argv);

    rc = GetEnvironmentVariableA("WINEHUA_T_TEST_KEY", buffer, sizeof(buffer));
    t_check("injected-key-present", rc > 0, "rc=%lu err=%lu", rc, rc == 0 ? GetLastError() : 0);
    t_check("injected-key-value", rc == (DWORD)lstrlenA("hello42") &&
            lstrcmpA(buffer, "hello42") == 0,
            "got '%s' (expect 'hello42')", rc ? buffer : "(null)");

    rc = GetEnvironmentVariableA("WINEHUA_T_ABSENT_KEY_XYZ", buffer, sizeof(buffer));
    t_check("absent-key-absent", rc == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND,
            "rc=%lu err=%lu", rc, GetLastError());

    /* 全量遍历：键名非空且无重复 */
    block = GetEnvironmentStringsA();
    if (block)
    {
        static char seen[96][64];
        int seen_n = 0;
        cursor = block;
        while (*cursor)
        {
            char name[64];
            char *eq = strchr(cursor, '=');
            size_t name_len;
            int j, dup = 0;
            if (eq && eq != cursor)
            {
                name_len = (size_t)(eq - cursor);
                if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
                memcpy(name, cursor, name_len);
                name[name_len] = '\0';
                for (j = 0; j < seen_n; ++j)
                {
                    if (!lstrcmpA(seen[j], name))
                    {
                        dup = 1;
                        break;
                    }
                }
                if (dup) dup_found++;
                else if (seen_n < 96)
                {
                    lstrcpynA(seen[seen_n++], name, sizeof(seen[0]));
                }
                count++;
            }
            cursor += strlen(cursor) + 1;
        }
        FreeEnvironmentStringsA(block);
    }
    else
        t_check("env-block", 0, "GetEnvironmentStringsA returned NULL");
    t_check("env-no-duplicates", dup_found == 0, "%d duplicated keys", dup_found);
    t_metric("env_count", "%d", count);
    return t_finish();
}
