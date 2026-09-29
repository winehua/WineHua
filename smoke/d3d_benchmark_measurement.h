#ifndef WINEHUA_D3D_BENCHMARK_MEASUREMENT_H
#define WINEHUA_D3D_BENCHMARK_MEASUREMENT_H
#include "benchmark_statistics.h"

#define BENCHMARK_MAX_SAMPLES 200000u
typedef struct D3DBenchmark {
    DWORD warmup_ms;
    unsigned draws, target_fps;
    int small_scissor, active, finished, failed, cpu_available;
    unsigned cpu_error;
    unsigned count, over_16_ms, over_33_ms;
    unsigned cpu_process_id;
    LARGE_INTEGER previous, begin, end, present_begin;
    double *frame_ms, *present_ms;
    double total_ms, present_total_ms, last_present_ms, next_frame_time;
    ULONGLONG cpu_begin, cpu_end, start_tick, end_tick;
} D3DBenchmark;
static D3DBenchmark g_benchmark;

static unsigned benchmark_env(const char *name, unsigned fallback, unsigned maximum) {
    const char *text = getenv(name);
    char *end;
    unsigned long value;
    if (!text || !*text) return fallback;
    value = strtoul(text, &end, 10);
    if (*end || value > maximum) { g_benchmark.failed = 1; return fallback; }
    return (unsigned)value;
}

static int benchmark_cpu(ULONGLONG *value) {
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        g_benchmark.cpu_error = GetLastError();
        return 0;
    }
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    *value = k.QuadPart + u.QuadPart;
    return 1;
}

static void benchmark_initialize(void) {
    g_benchmark.warmup_ms = benchmark_env("WINEHUA_BENCHMARK_WARMUP_MS", 15000, 60000);
    g_benchmark.draws = benchmark_env("WINEHUA_BENCHMARK_DRAWS", 1, 10000);
    g_benchmark.target_fps = benchmark_env("WINEHUA_BENCHMARK_FPS", 0, 240);
    g_benchmark.small_scissor = benchmark_env("WINEHUA_BENCHMARK_SMALL_SCISSOR", 1, 1);
    g_benchmark.cpu_process_id = GetCurrentProcessId();
    g_benchmark.frame_ms = calloc(BENCHMARK_MAX_SAMPLES, sizeof(double));
    g_benchmark.present_ms = calloc(BENCHMARK_MAX_SAMPLES, sizeof(double));
    if (!g_app.duration_ms) g_app.duration_ms = 75000;
    if (!g_benchmark.draws || !g_benchmark.frame_ms || !g_benchmark.present_ms ||
        g_app.duration_ms <= g_benchmark.warmup_ms + 1000 || g_app.resize_test)
        g_benchmark.failed = 1;
}

static void benchmark_window(const char *phase) {
    char path[MAX_PATH + 40], temporary[MAX_PATH + 48];
    FILE *file;
    snprintf(path, sizeof(path), "%s.perf-window.json", g_app.result_path);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    file = fopen(temporary, "wb");
    if (!file) { g_benchmark.failed = 1; return; }
    fprintf(file, "{\"phase\":\"%s\",\"runId\":\"%s\",\"windowsPid\":%u,"
                  "\"startTickMs\":%llu,\"endTickMs\":%llu,\"measuredFrames\":%u}\n",
                  phase, g_app.run_id, g_benchmark.cpu_process_id,
                  g_benchmark.start_tick, g_benchmark.end_tick, g_benchmark.count);
    fclose(file);
    if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        g_benchmark.failed = 1;
}

static void benchmark_frame_complete(void) {
    LARGE_INTEGER now;
    double milliseconds;
    if (g_benchmark.failed || !g_app.renderer_ready) { g_app.running = 0; return; }
    if (FAILED(g_app.present_result) || g_app.width != g_app.initial_width || g_app.height != g_app.initial_height) {
        g_benchmark.failed = 1; g_app.running = 0; return;
    }
    QueryPerformanceCounter(&now);
    if (!g_benchmark.active) {
        const double elapsed = (double)(now.QuadPart - g_app.start_qpc.QuadPart) * 1000 / g_app.qpc_freq.QuadPart;
        if (elapsed < g_benchmark.warmup_ms) return;
        g_benchmark.active = 1;
        // Emit the one-time boundary before recording CPU/QPC, so file IO is
        // excluded from the measured interval. No per-frame disk writes.
        g_benchmark.start_tick = GetTickCount64();
        benchmark_window("measuring");
        g_benchmark.cpu_available = benchmark_cpu(&g_benchmark.cpu_begin);
        QueryPerformanceCounter(&g_benchmark.begin);
        g_benchmark.previous = g_benchmark.begin;
        return;
    }
    milliseconds = (double)(now.QuadPart - g_benchmark.previous.QuadPart) * 1000 / g_app.qpc_freq.QuadPart;
    if (g_benchmark.count == BENCHMARK_MAX_SAMPLES || !isfinite(milliseconds) || milliseconds <= 0) {
        g_benchmark.failed = 1; g_app.running = 0; return;
    }
    g_benchmark.frame_ms[g_benchmark.count] = milliseconds;
    g_benchmark.present_ms[g_benchmark.count] = g_benchmark.last_present_ms;
    g_benchmark.total_ms += milliseconds;
    g_benchmark.present_total_ms += g_benchmark.present_ms[g_benchmark.count];
    g_benchmark.over_16_ms += milliseconds > 1000.0 / 60;
    g_benchmark.over_33_ms += milliseconds > 1000.0 / 30;
    ++g_benchmark.count;
    g_benchmark.previous = now;
    g_benchmark.end = now;
}

static void benchmark_limit(void) {
    if (g_benchmark.target_fps && g_app.running) {
        LARGE_INTEGER now;
        // One absolute schedule from the renderer-ready epoch avoids timing
        // drift. If rendering falls behind, never issue catch-up bursts.
        QueryPerformanceCounter(&now);
        const double elapsed = (double)(now.QuadPart - g_app.start_qpc.QuadPart) / g_app.qpc_freq.QuadPart;
        g_benchmark.next_frame_time += 1.0 / g_benchmark.target_fps;
        if (g_benchmark.next_frame_time <= elapsed) {
            g_benchmark.next_frame_time = elapsed;
            return;
        }
        if (g_benchmark.next_frame_time > elapsed) {
            const DWORD wait = (DWORD)ceil((g_benchmark.next_frame_time - elapsed) * 1000);
            if (wait) Sleep(wait);
        }
    }
}

static void benchmark_finish(void) {
    g_benchmark.finished = 1;
    g_benchmark.end_tick = GetTickCount64();
    if (g_benchmark.cpu_available) {
        g_benchmark.cpu_available = benchmark_cpu(&g_benchmark.cpu_end);
        if (g_benchmark.cpu_end < g_benchmark.cpu_begin) g_benchmark.cpu_available = 0;
    }
    // Cap/quit/resize/failed Present produces a FAIL, even with old good frames.
    if (!g_benchmark.active || g_benchmark.total_ms < g_app.duration_ms - g_benchmark.warmup_ms - 1000 ||
        g_benchmark.count < 60) g_benchmark.failed = 1;
    benchmark_window("complete");
}

static void benchmark_write(FILE *file) {
    const double measured = g_benchmark.total_ms;
    double p50 = 0, p95 = 0, p99 = 0, present95 = 0, present99 = 0;
    if (g_benchmark.finished && g_benchmark.count) {
        char path[MAX_PATH + 32];
        unsigned i;
        FILE *raw;
        snprintf(path, sizeof(path), "%s.frames.csv", g_app.result_path);
        raw = fopen(path, "wb");
        if (raw) {
            fputs("frame,frameMs,presentMs\n", raw);
            for (i = 0; i < g_benchmark.count; ++i)
                fprintf(raw, "%u,%.9f,%.9f\n", i + 1, g_benchmark.frame_ms[i], g_benchmark.present_ms[i]);
            fclose(raw);
        }
        qsort(g_benchmark.frame_ms, g_benchmark.count, sizeof(double), benchmark_double_compare);
        qsort(g_benchmark.present_ms, g_benchmark.count, sizeof(double), benchmark_double_compare);
        p50 = benchmark_percentile(g_benchmark.frame_ms, g_benchmark.count, .50);
        p95 = benchmark_percentile(g_benchmark.frame_ms, g_benchmark.count, .95);
        p99 = benchmark_percentile(g_benchmark.frame_ms, g_benchmark.count, .99);
        present95 = benchmark_percentile(g_benchmark.present_ms, g_benchmark.count, .95);
        present99 = benchmark_percentile(g_benchmark.present_ms, g_benchmark.count, .99);
    }
    fprintf(file, ",\n  \"benchmark\": {\"complete\":%s,\"valid\":%s,\"peMachine\":\"%s\","
                  "\"vulkanBackend\":\"%s\",\"warmupMs\":%lu,\"configuredRunMs\":%lu,"
                  "\"drawsPerFrame\":%u,\"smallScissor\":%s,\"targetFps\":%u,"
                  "\"measuredFrames\":%u,\"measuredMs\":%.9f,\"averageFps\":%.9f,"
                  "\"frameMsP50\":%.9f,\"frameMsP95\":%.9f,\"frameMsP99\":%.9f,"
                  "\"over16_67Ms\":%u,\"over33_33Ms\":%u,\"presentMeanMs\":%.9f,"
                  "\"presentMsP95\":%.9f,\"presentMsP99\":%.9f,"
                  "\"cpuAvailable\":%s,\"cpuQueryError\":%u,\"cpuMs\":",
                  g_benchmark.finished ? "true" : "false", g_benchmark.finished && !g_benchmark.failed ? "true" : "false",
#if defined(__aarch64__) || defined(_M_ARM64)
                  "ARM64",
#else
                  "AMD64",
#endif
                  getenv("WINEHUA_VULKAN_BACKEND") ? getenv("WINEHUA_VULKAN_BACKEND") : "unknown",
                  (unsigned long)g_benchmark.warmup_ms, (unsigned long)g_app.duration_ms,
                  g_benchmark.draws, g_benchmark.small_scissor ? "true" : "false", g_benchmark.target_fps,
                  g_benchmark.count, measured, measured > 0 ? g_benchmark.count * 1000 / measured : 0,
                  p50, p95, p99, g_benchmark.over_16_ms, g_benchmark.over_33_ms,
                  g_benchmark.count ? g_benchmark.present_total_ms / g_benchmark.count : 0, present95, present99,
                  g_benchmark.cpu_available ? "true" : "false", g_benchmark.cpu_error);
    if (g_benchmark.finished && g_benchmark.cpu_available)
        fprintf(file, "%.6f", (double)(g_benchmark.cpu_end - g_benchmark.cpu_begin) / 10000);
    else fputs("null", file);
    fputs(",\"cpuMsPerFrame\":", file);
    if (g_benchmark.finished && g_benchmark.cpu_available && g_benchmark.count)
        fprintf(file, "%.9f", (double)(g_benchmark.cpu_end - g_benchmark.cpu_begin) / 10000 / g_benchmark.count);
    else fputs("null", file);
    fputc('}', file);
}
#endif
