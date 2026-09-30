/* winehua_t_crt — C 运行时（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.11。
 * 失败特征：断 = msvcrt 映射/locale 断。
 */
#include "../common/winehua_t_check.h"
#include <stdlib.h>
#include <wchar.h>

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(int argc, char **argv)
{
    char buffer[128];
    wchar_t wbuf[64];
    double value;
    long parsed;
    int i;

    t_begin("winehua_t_crt", argc, argv);

    /* printf 家族格式化 */
    i = _snprintf(buffer, sizeof(buffer), "%d %s %c %05.2f %X",
                  -42, "str", 'Z', 3.14159, 48879);
    t_check("snprintf-format", i > 0 && !strcmp(buffer, "-42 str Z 03.14 BEEF"),
            "got '%s'", buffer);

    /* 宽窄转换往返 */
    {
        const char *src = "WineHuaT CRT 42";
        size_t n = mbstowcs(wbuf, src, 63);
        char back[64];
        size_t m = 0;
        t_check("mbstowcs-len", n == (size_t)lstrlenA(src), "n=%lu", (unsigned long)n);
        if (n != (size_t)-1)
        {
            m = wcstombs(back, wbuf, sizeof(back) - 1);
            back[m ? m : 0] = '\0';
        }
        t_check("wcstombs-roundtrip", m == (size_t)lstrlenA(src) && !strcmp(back, src),
                "back='%s'", back);
    }

    /* 数值解析与格式化往返 */
    value = atof("-123.75");
    t_check("atof", value == -123.75, "got %f", value);
    parsed = atol("987654");
    t_check("atol", parsed == 987654, "got %ld", parsed);
    _itoa(parsed, buffer, 16);
    t_check("itoa-hex", !strcmp(buffer, "f1206"), "got '%s'", buffer);

    /* 内存块函数 */
    {
        unsigned char *block = (unsigned char *)malloc(1024);
        t_check("malloc", block != NULL, "null");
        if (block)
        {
            memset(block, 0xAB, 1024);
            t_check("memset-memchr", memchr(block, 0xAB, 1024) == (void *)block, "memchr");
            block[512] = 0xCD;
            t_check("write-visible", block[512] == 0xCD, "value");
            free(block);
        }
        block = (unsigned char *)calloc(64, 1);
        t_check("calloc-zeroed", block && block[0] == 0 && block[63] == 0, "not zeroed");
        free(block);
    }

    /* qsort/bsearch */
    {
        int arr[16];
        int key = 9;
        int *found;
        for (i = 0; i < 16; ++i)
            arr[i] = (i * 7) % 16;
        qsort(arr, 16, sizeof(int), cmp_int);
        t_check("qsort-sorted", arr[0] == 0 && arr[15] == 15 && arr[1] <= arr[2],
                "head=%d tail=%d", arr[0], arr[15]);
        found = (int *)bsearch(&key, arr, 16, sizeof(int), cmp_int);
        t_check("bsearch-found", found && *found == 9,
                "found=%s", found ? "yes" : "no");
    }
    return t_finish();
}
