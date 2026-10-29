/* winehua_t_* 测试程序共用断言框架（设计见 docs/engineering/testing-programs.md）。
 *
 * 用法：
 *   #include "../common/winehua_t_check.h"
 *   int main(int argc, char **argv) {
 *       t_begin("winehua_t_xxx", argc, argv);
 *       t_check("step-name", cond != 0, "detail %d", value);
 *       t_metric("key", "%d", v);
 *       return t_finish();
 *   }
 *
 * 约定：任一 t_check 失败不中断，收集全部结论后 t_finish 统一写 result；
 * 退出码 0=全过 1=有失败（手跑模式可直接看退出码）。
 * result JSON 顶层 status/stage/message/metrics 与 winehua_smoke_protocol.h
 * 的设备端协议对齐（判定器 result_json 只认终态 status），另加 checks[]
 * 数组供归档后人工/工具细读。
 */
#ifndef WINEHUA_T_CHECK_H
#define WINEHUA_T_CHECK_H

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "winehua_smoke_protocol.h"

#define T_MAX_CHECKS 192
#define T_MAX_METRICS 48

struct t_check_entry
{
    char name[96];
    BOOL pass;
    char detail[220];
};

struct t_metric_entry
{
    char key[64];
    char value[140];
};

struct t_state
{
    struct winehua_smoke_options options;
    struct t_check_entry checks[T_MAX_CHECKS];
    int n_checks;
    struct t_metric_entry metrics[T_MAX_METRICS];
    int n_metrics;
    int failed;
    int skipped;
    char skip_reason[220];
};

static struct t_state g_t;

/* 参数解析：--result/--test-id/--automation 走公共协议头，未知参数忽略。 */
static void t_begin(const char *default_test_id, int argc, char **argv)
{
    memset(&g_t, 0, sizeof(g_t));
    winehua_smoke_parse_options(&g_t.options, argc, argv, 30);
    if (!g_t.options.test_id[0] || !lstrcmpiA(g_t.options.test_id, "unknown"))
        winehua_smoke_copy_arg(g_t.options.test_id, sizeof(g_t.options.test_id), default_test_id);
}

/* 记录一条断言。pass=0 记失败；detail 支持 printf 风格。 */
static void t_check(const char *name, int pass, const char *detail_fmt, ...)
{
    struct t_check_entry *entry;
    va_list args;

    if (g_t.n_checks >= T_MAX_CHECKS)
        return;
    entry = &g_t.checks[g_t.n_checks++];
    winehua_smoke_copy_arg(entry->name, sizeof(entry->name), name);
    entry->pass = pass ? TRUE : FALSE;
    if (!pass)
        ++g_t.failed;
    va_start(args, detail_fmt);
    _vsnprintf(entry->detail, sizeof(entry->detail), detail_fmt, args);
    entry->detail[sizeof(entry->detail) - 1] = '\0';
    va_end(args);
}

/* 便捷宏：断言表达式本身作为通过描述。 */
#define TCHECK(name, expr) t_check((name), (expr) ? 1 : 0, "%s", #expr)
/* 单参形式：断言表达式同时充当名字与描述，适合一次性布尔调用。 */
#define TEXPR(expr) t_check(#expr, (expr) ? 1 : 0, "%s", #expr)

static void t_metric(const char *key, const char *fmt, ...)
{
    struct t_metric_entry *entry;
    va_list args;

    if (g_t.n_metrics >= T_MAX_METRICS)
        return;
    entry = &g_t.metrics[g_t.n_metrics++];
    winehua_smoke_copy_arg(entry->key, sizeof(entry->key), key);
    va_start(args, fmt);
    _vsnprintf(entry->value, sizeof(entry->value), fmt, args);
    entry->value[sizeof(entry->value) - 1] = '\0';
    va_end(args);
}

/* 标记整例 SKIP（能力不足时用，判定器按 SKIP 透传，不算失败）。 */
static void t_skip(const char *reason)
{
    g_t.skipped = 1;
    winehua_smoke_copy_arg(g_t.skip_reason, sizeof(g_t.skip_reason), reason);
}

/* 创建顶层窗口：优先内置控件类；smoke 自动化会话里 win32u 的内置类注册
 * 只挂在桌面初始化链上（连已有桌面时不触发），内置类可能 1411，此时回退
 * 自注册类并记 metric——保证各例主断言不被这个无关缺口掩盖。 */
static HWND t_create_toplevel(const char *builtin_class, const char *fallback_class,
                              DWORD style, DWORD ex_style, int x, int y, int w, int h,
                              const char *title)
{
    WNDCLASSA wc;
    HWND hwnd = CreateWindowExA(ex_style, builtin_class, title, style, x, y, w, h,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (hwnd)
        return hwnd;
    t_metric("builtin-class-fallback", "%s err=%lu", builtin_class, GetLastError());
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = fallback_class;
    RegisterClassA(&wc);
    return CreateWindowExA(ex_style, fallback_class, title, style, x, y, w, h,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
}

/* result JSON 必须是合法 UTF-8（主机侧按 utf-8 解析）；A 系列 API 在中文
 * locale 下产出 GBK 字节，这里把 >=0x80 的字节逐个转成 \u00XX，输出保持
 * 纯 ASCII，字节值经 latin-1 码元无损保留。 */
static void t_write_json_string(FILE *file, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    fputc('"', file);
    while (*cursor)
    {
        unsigned char c = *cursor;
        switch (c)
        {
        case '"': fputs("\\\"", file); break;
        case '\\': fputs("\\\\", file); break;
        case '\n': fputs("\\n", file); break;
        case '\r': fputs("\\r", file); break;
        case '\t': fputs("\\t", file); break;
        default:
            if (c < 0x20 || c >= 0x80) fprintf(file, "\\u%04x", (unsigned)c);
            else fputc(c, file);
            break;
        }
        ++cursor;
    }
    fputc('"', file);
}

static const char *t_status(void)
{
    if (g_t.skipped) return "SKIP";
    return g_t.failed ? "FAIL" : "PASS";
}

/* 汇总写 result（原子写：tmp + MoveFile，同协议头模板）。 */
static int t_finish(void)
{
    char temporary[MAX_PATH];
    FILE *file;
    const char *status = t_status();
    char message[400];
    int i, offset;
    BOOL written = TRUE;

    if (g_t.skipped)
        winehua_smoke_copy_arg(message, sizeof(message), g_t.skip_reason);
    else if (g_t.failed)
    {
        offset = _snprintf(message, sizeof(message), "%d/%d checks failed:",
                           g_t.failed, g_t.n_checks);
        for (i = 0; i < g_t.n_checks && offset > 0 && offset < (int)sizeof(message) - 2; ++i)
        {
            if (!g_t.checks[i].pass)
            {
                int room = (int)sizeof(message) - offset;
                int written_len = _snprintf(message + offset, room, " %s;",
                                            g_t.checks[i].name);
                if (written_len < 0) break;
                offset += written_len;
            }
        }
        message[sizeof(message) - 1] = '\0';
    }
    else
        _snprintf(message, sizeof(message), "%d/%d checks passed",
                  g_t.n_checks, g_t.n_checks), message[sizeof(message) - 1] = '\0';

    if (g_t.options.automation && !g_t.options.result_path[0])
    {
        printf("[%s] %s\n", status, message);
        return g_t.failed ? 1 : 0;
    }
    if (!g_t.options.result_path[0])
    {
        printf("[%s] %s\n", status, message);
        for (i = 0; i < g_t.n_checks; ++i)
            printf("  [%s] %s: %s\n", g_t.checks[i].pass ? "OK" : "FAIL",
                   g_t.checks[i].name, g_t.checks[i].detail);
        return g_t.failed ? 1 : 0;
    }

    if (!winehua_smoke_ensure_parent(g_t.options.result_path))
        return 2;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", g_t.options.result_path,
             (unsigned long)GetCurrentProcessId());
    file = fopen(temporary, "wb");
    if (!file)
    {
        fprintf(stderr, "winehua_t: cannot open result file %s\n", g_t.options.result_path);
        return 2;
    }

    fputs("{\n  \"schemaVersion\": 1,\n  \"runId\": ", file);
    t_write_json_string(file, g_t.options.run_id);
    fputs(",\n  \"testId\": ", file);
    t_write_json_string(file, g_t.options.test_id);
    fprintf(file, ",\n  \"status\": \"%s\",\n  \"stage\": \"winehua-t\",\n  \"message\": ",
            status);
    t_write_json_string(file, message);
    fprintf(file, ",\n  \"pid\": %lu,\n  \"heartbeatTimestampMs\": %llu,\n  \"metrics\": {",
            (unsigned long)GetCurrentProcessId(), winehua_smoke_timestamp_ms());
    for (i = 0; i < g_t.n_metrics; ++i)
    {
        fprintf(file, "%s\n    ", i ? "," : "");
        t_write_json_string(file, g_t.metrics[i].key);
        fputs(": ", file);
        t_write_json_string(file, g_t.metrics[i].value);
    }
    fprintf(file, "\n  },\n  \"failedCount\": %d,\n  \"totalCount\": %d,\n  \"checks\": [",
            g_t.failed, g_t.n_checks);
    for (i = 0; i < g_t.n_checks; ++i)
    {
        fprintf(file, "%s\n    {\"name\": ", i ? "," : "");
        t_write_json_string(file, g_t.checks[i].name);
        fprintf(file, ", \"pass\": %s, \"detail\": ",
                g_t.checks[i].pass ? "true" : "false");
        t_write_json_string(file, g_t.checks[i].detail);
        fputc('}', file);
    }
    fputs("\n  ]\n}\n", file);

    if (fflush(file))
        written = FALSE;
    fclose(file);
    if (written)
    {
        if (!MoveFileExA(temporary, g_t.options.result_path, MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileA(temporary);
            written = FALSE;
        }
    }
    else
        DeleteFileA(temporary);
    if (!written)
    {
        fprintf(stderr, "winehua_t: failed writing %s\n", g_t.options.result_path);
        return 2;
    }
    return g_t.failed ? 1 : 0;
}

#endif /* WINEHUA_T_CHECK_H */
