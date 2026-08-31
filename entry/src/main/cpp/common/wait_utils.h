/**
 * wait_utils.h — 通用 readiness probe 工具
 *
 * 用轮询 probe() 替代盲等 usleep。probe 是 readiness signal 的检查函数
 * (如文件存在、socket 可连接等)。正常路径由 signal 驱动，超时仅作安全网。
 *
 * 注意: 作为 header-only 工具，不重定义 LOG_TAG (避免污染 include 者)。
 * 日志通过 [wait] 前缀识别。
 */

#ifndef WINE_WAIT_UTILS_H
#define WINE_WAIT_UTILS_H

#include <functional>
#include <unistd.h>
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#include <hilog/log.h>

// 轮询 probe() 直到返回 true 或超时。
// intervalMs: 轮询间隔。timeoutMs: 总超时。
// 返回 true = probe 成功，false = 超时。
static bool WaitFor(const char* description,
                    std::function<bool()> probe,
                    int timeoutMs = 5000,
                    int intervalMs = 100) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += intervalMs) {
        if (probe()) {
            if (elapsed > 0) {
                OH_LOG_INFO(LOG_APP, "[wait] %{public}s ready (%{public}d ms)",
                            description, elapsed);
            }
            return true;
        }
        usleep(intervalMs * 1000);
    }
    OH_LOG_WARN(LOG_APP, "[wait] %{public}s TIMEOUT after %{public}d ms",
                description, timeoutMs);
    return false;
}

#endif // WINE_WAIT_UTILS_H
