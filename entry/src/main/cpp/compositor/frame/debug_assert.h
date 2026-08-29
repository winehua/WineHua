#pragma once

// ============================================================================
// MW_ASSERT — 不变式断言 (默认编译为空)
//
// 不用标准 assert: OHOS NDK 默认构建不带 NDEBUG, 标准 assert 在设备上
// 是 live 的, 误触发即 abort。本宏只在显式加 -DWINEHUA_DEBUG_ASSERT 的
// 调试构建中启用, 触发即 FATAL 日志 + __builtin_trap。
//
// 使用纪律: 只用于 "审计确认恒真" 的维护守卫 (防未来改动破坏);
// 设计规则类不变式 (如 root 不参与可见性判定) 由代码契约强制,
// 写 owning class 头注释, 不加断言。
// ============================================================================

#ifdef WINEHUA_DEBUG_ASSERT
#include <hilog/log.h>
#define MW_ASSERT(cond, msg)                                                        \
    do {                                                                            \
        if (!(cond)) {                                                              \
            OH_LOG_FATAL(LOG_APP, "[MW-ASSERT] %{public}s (%{public}s:%{public}d)", \
                         msg, __FILE__, __LINE__);                                  \
            __builtin_trap();                                                       \
        }                                                                           \
    } while (0)
#else
#define MW_ASSERT(cond, msg) ((void)0)
#endif
