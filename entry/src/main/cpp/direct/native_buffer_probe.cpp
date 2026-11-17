#include "native_buffer_probe.h"
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace winehua {
namespace {
std::string JsonString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 32) out += static_cast<char>(c);
    }
    return out + "\"";
}
}
std::string NativeBufferMemoryProbeJson()
{
    void* module = dlopen("libwinehua_native_memory_probe.so", RTLD_NOW | RTLD_LOCAL);
    using Probe = int (*)(void);
    const auto probe = module ? reinterpret_cast<Probe>(dlsym(module, "WinehuaNativeBufferMemoryMain")) : nullptr;
    if (!probe) {
        if (module) dlclose(module);
        return "{\"schemaVersion\":1,\"status\":\"native_buffer_probe_load_failed\"}";
    }
    struct CloseModule { void* handle; ~CloseModule() { dlclose(handle); } } closeModule{module};
    const char* path = "/data/storage/el2/base/temp/native-buffer-memory-probe.log";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat status{};
    if (fd < 0 || fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_uid != getuid()) {
        if (fd >= 0) close(fd);
        return "{\"schemaVersion\":1,\"status\":\"native_buffer_probe_log_failed\"}";
    }
    const int previousError = dup(STDERR_FILENO);
    if (dup2(fd, STDERR_FILENO) < 0) {
        if (previousError >= 0) close(previousError);
        close(fd);
        return "{\"schemaVersion\":1,\"status\":\"native_buffer_probe_capture_failed\"}";
    }
    const int previous = dup(STDOUT_FILENO);
    if (dup2(fd, STDOUT_FILENO) < 0) {
        if (previous >= 0) close(previous);
        if (previousError >= 0) { dup2(previousError, STDERR_FILENO); close(previousError); }
        close(fd);
        return "{\"schemaVersion\":1,\"status\":\"native_buffer_probe_capture_failed\"}";
    }
    const int code = probe();
    fflush(stdout); fflush(stderr);
    if (previousError >= 0) { dup2(previousError, STDERR_FILENO); close(previousError); }
    else close(STDERR_FILENO);
    if (previous >= 0) { dup2(previous, STDOUT_FILENO); close(previous); }
    else close(STDOUT_FILENO);
    // The probe has four fixed cases and no variable-length logging. Keep
    // the result bounded even if a driver adds messages on stdout.
    char bytes[65536];
    const ssize_t length = pread(fd, bytes, sizeof(bytes), 0);
    fstat(fd, &status);
    close(fd);
    const std::string output = length > 0 ? std::string(bytes, static_cast<size_t>(length)) : "";
    return "{\"schemaVersion\":1,\"status\":\"native_buffer_memory_completed\",\"nativeBufferProbeExit\":" +
        std::to_string(code) + ",\"nativeBufferProbeOutput\":" + JsonString(output) +
        ",\"nativeBufferProbeTruncated\":" + (status.st_size > static_cast<off_t>(sizeof(bytes)) ? "true" : "false") + '}';
}
}
