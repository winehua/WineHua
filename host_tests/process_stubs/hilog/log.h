#pragma once
void ProcessTestLog(const char* format);
template<class... Args> inline void HostTestLog(int, const char* format, Args&&...) {
    ProcessTestLog(format);
}
#define LOG_APP 0
#define OH_LOG_INFO(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_WARN(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_ERROR(...) HostTestLog(__VA_ARGS__)
#define OH_LOG_FATAL(...) HostTestLog(__VA_ARGS__)
