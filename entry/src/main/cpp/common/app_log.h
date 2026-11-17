#pragma once

/*
 * WineHua app log sink header.
 *
 * This header is force-included (CMake: -include app_log.h) into every TU of
 * the entry target so that all existing OH_LOG_* call sites keep compiling
 * unchanged while also appending a formatted line to the app-scoped Download
 * logs directory (native-YYYYMMDD[-n].log). The real hilog behavior is
 * preserved: each macro still calls OH_LOG_Print with the same arguments.
 *
 * Keep this file C-compatible (the -include flag also applies to .c files).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <hilog/log.h>

#undef OH_LOG_DEBUG
#undef OH_LOG_INFO
#undef OH_LOG_WARN
#undef OH_LOG_ERROR
#undef OH_LOG_FATAL

#define OH_LOG_DEBUG(type, ...) \
    do { \
        (void)OH_LOG_Print((type), LOG_DEBUG, LOG_DOMAIN, LOG_TAG, __VA_ARGS__); \
        WineHuaLogAppend(LOG_DEBUG, LOG_TAG, __VA_ARGS__); \
    } while (0)

#define OH_LOG_INFO(type, ...) \
    do { \
        (void)OH_LOG_Print((type), LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__); \
        WineHuaLogAppend(LOG_INFO, LOG_TAG, __VA_ARGS__); \
    } while (0)

#define OH_LOG_WARN(type, ...) \
    do { \
        (void)OH_LOG_Print((type), LOG_WARN, LOG_DOMAIN, LOG_TAG, __VA_ARGS__); \
        WineHuaLogAppend(LOG_WARN, LOG_TAG, __VA_ARGS__); \
    } while (0)

#define OH_LOG_ERROR(type, ...) \
    do { \
        (void)OH_LOG_Print((type), LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__); \
        WineHuaLogAppend(LOG_ERROR, LOG_TAG, __VA_ARGS__); \
    } while (0)

#define OH_LOG_FATAL(type, ...) \
    do { \
        (void)OH_LOG_Print((type), LOG_FATAL, LOG_DOMAIN, LOG_TAG, __VA_ARGS__); \
        WineHuaLogAppend(LOG_FATAL, LOG_TAG, __VA_ARGS__); \
    } while (0)

/* Appends a formatted line to the native log sink. */
void WineHuaLogAppend(int level, const char* tag, const char* fmt, ...);

/* Points the sink at the logs directory and flushes the pre-init ring buffer. */
void WineHuaLogInit(const char* dirPath);

/* Directory passed to WineHuaLogInit, or empty if the sink is not ready. */
void WineHuaLogDirectory(char* out, size_t outSize);

/* Closes the sink and removes every native-*.log file in the logs directory. */
void WineHuaLogClear(void);

#ifdef __cplusplus
}
#endif
