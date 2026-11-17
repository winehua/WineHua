#pragma once
// Preserve argument evaluation/type checking without depending on OHOS logging.
template<class... Args> inline void HostTestLog(Args&&...) {}
#define LOG_APP 0
#define OH_LOG_INFO(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_DEBUG(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_WARN(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_ERROR(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_FATAL(...) HostTestLog(__VA_ARGS__)
