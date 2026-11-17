#ifndef WINE_SCHEME_H
#define WINE_SCHEME_H

// -- 构建方案标识 (编译期钉死, 运行期只读) --
// 方案判定宏的组合逻辑必须与 wine_constants.h / wine_child.cpp / graphics_broker.cpp
// 保持一致; 用于启动日志确认 libentry.so / libwine_child.so 实际命中了哪个宏分支
// (hvigor daemon 缓存曾导致宏未随 WINE_ARCH 重注入 → 方案错配, 见 package.sh 注释)。

#include <hilog/log.h>

#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
#define WINEHUA_SCHEME_NAME  "方案② box64+wine (__aarch64__ + WINEHUA_WINE_ARCH_IS_X86_64)"
#define WINEHUA_SCHEME_ID    2
#elif defined(__aarch64__)
#define WINEHUA_SCHEME_NAME  "方案③ arm64 原生 wine (__aarch64__)"
#define WINEHUA_SCHEME_ID    3
#elif defined(__x86_64__)
#define WINEHUA_SCHEME_NAME  "方案① x86_64 原生 wine (__x86_64__)"
#define WINEHUA_SCHEME_ID    1
#else
#define WINEHUA_SCHEME_NAME  "未知方案 (编译宏异常!)"
#define WINEHUA_SCHEME_ID    0
#endif

static inline void LogWineScheme(const char* where) {
    OH_LOG_INFO(LOG_APP, "[Scheme] %{public}s → %{public}s", where, WINEHUA_SCHEME_NAME);
}

#endif // WINE_SCHEME_H
