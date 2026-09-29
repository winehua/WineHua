#include "direct_shared_buffer_probe.h"
#include "native_buffer_socket.h"
#include "direct_buffer_import_probe.h"
#include "phone_adapter/phone_process.h"

#include <native_buffer/native_buffer.h>
#include <native_buffer/buffer_common.h>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <cerrno>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace winehua::direct {
namespace {
using namespace shared;
std::mutex probeMutex, systemExitMutex;
std::unordered_map<int32_t, int32_t> systemExits;
void OnSystemExit(int32_t pid, int32_t signal) {
    std::lock_guard<std::mutex> lock(systemExitMutex);
    systemExits[pid] = signal;
}
struct SystemObserver {
    void* library = dlopen("libchild_process.so", RTLD_NOW | RTLD_LOCAL);
    decltype(&OH_Ability_StartNativeChildProcess) start = nullptr;
    decltype(&OH_Ability_UnregisterNativeChildProcessExitCallback) unregister = nullptr;
    bool registered = false;
    SystemObserver() {
        if (!library) return;
        start = reinterpret_cast<decltype(start)>(dlsym(library, "OH_Ability_StartNativeChildProcess"));
        auto reg = reinterpret_cast<decltype(&OH_Ability_RegisterNativeChildProcessExitCallback)>(
            dlsym(library, "OH_Ability_RegisterNativeChildProcessExitCallback"));
        unregister = reinterpret_cast<decltype(unregister)>(
            dlsym(library, "OH_Ability_UnregisterNativeChildProcessExitCallback"));
        if (reg && unregister) registered = reg(OnSystemExit) == NCP_NO_ERROR;
    }
    ~SystemObserver() {
        if (registered) unregister(OnSystemExit);
        if (library) dlclose(library);
    }
};
struct Work {
    napi_async_work async = nullptr;
    napi_deferred deferred = nullptr;
    bool passed = false;
    const char* stage = "pending";
    int32_t result = 0;
    int32_t systemError = 0;
    int32_t child = -1;
    int32_t server = -1;
    int32_t launchCode = -1;
    int32_t childExit = -1;
    int32_t childWaitStatus = -1;
    int32_t childSignal = 0;
    int diagnosticFd = -1;
    bool reaped = false;
    bool exitObserved = false;
    bool localRoundtrip = false;
    bool preserveLowMappings = false;
    bool standardFork = false;
    bool systemStart = false;
    bool guestExport = false;
    std::unique_ptr<SystemObserver> observer;
    bool childKilledByProbe = false;
    bool childDeviceCreated = false;
    int32_t parentFdsBefore = -1;
    int32_t parentFdsAfter = -1;
    uint32_t backingFds = 0, originalFieldCount = 0;
    uint32_t frames = 0, childImports = 0, childFenceImports = 0, childFenceExports = 0;
    uint32_t childAllocations = 0, childExports = 0, childRealFenceImports = 0, childRealFenceExports = 0;
    uint32_t hostRealFenceImports = 0, hostRealFenceExports = 0;
    uint32_t hostImports = 0, hostReuses = 0, hostFenceImports = 0, hostFenceExports = 0;
    char lastChildStage[64]{};
    char loaderPath[128]{};
    char deviceName[128]{};
    char childStderr[4096]{};
};
int CountFds() {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = -1;
    while (auto* item = readdir(directory)) if (item->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}
struct Buffer {
    OH_NativeBuffer* value = nullptr;
    ~Buffer() { if (value) OH_NativeBuffer_Unreference(value); }
};
struct Socket {
    int value = -1;
    ~Socket() { if (value >= 0) close(value); }
};

bool Wait(Work& w, int fd, Kind kind, Message& message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || !Receive(fd, message, static_cast<int>(remaining))) {
            w.stage = "child_packet_timeout_or_eof";
            return false;
        }
        const auto& p = message.packet;
        if (p.pid != w.child) { w.stage = "child_packet_pid"; return false; }
        snprintf(w.lastChildStage, sizeof(w.lastChildStage), "%s", p.stage);
        if (p.loaderPath[0]) snprintf(w.loaderPath, sizeof(w.loaderPath), "%s", p.loaderPath);
        if (p.deviceName[0]) snprintf(w.deviceName, sizeof(w.deviceName), "%s", p.deviceName);
        if (!strcmp(p.stage, "writer_device_ready")) w.childDeviceCreated = true;
        w.childImports = p.imports;
        w.childAllocations = p.allocations;
        w.childExports = p.exports;
        w.childFenceImports = p.fenceImports;
        w.childFenceExports = p.fenceExports;
        w.childRealFenceImports = p.realFenceImports;
        w.childRealFenceExports = p.realFenceExports;
        if (p.kind == Kind::Stage && message.fds.empty()) continue;
        if (p.kind == Kind::Error) { w.stage = "child_error"; w.result = p.result; return false; }
        if (p.kind != kind) { w.stage = "child_packet_kind"; return false; }
        return true;
    }
}

void Run(Work& w) {
    w.server = Phone_GetDirectForkServerPid();
    if (!w.systemStart && w.server <= 0) { w.stage = "early_fork_server_unavailable"; return; }
    Buffer buffer;
    Message descriptor;
    if (!w.guestExport) {
        OH_NativeBuffer_Config config{};
        config.width = config.height = 64;
        config.format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
        config.usage = NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_MEM_DMA;
        w.stage = "parent_allocate";
        buffer.value = OH_NativeBuffer_Alloc(&config);
        if (!buffer.value) return;
        if (!EncodeBuffer(buffer.value, descriptor, &w.stage)) return;
        w.backingFds = descriptor.packet.fdCount;
        w.originalFieldCount = descriptor.packet.originalFieldCount;
        Buffer roundtrip;
        if (!DecodeBuffer(descriptor, &roundtrip.value, &w.stage)) return;
        w.localRoundtrip = true;
    } else descriptor.packet.kind = Kind::Allocate;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) { w.stage = "socketpair"; return; }
    Socket parent{sockets[0]}, child{sockets[1]};
    int diagnostics[2];
    if (pipe2(diagnostics, O_CLOEXEC | O_NONBLOCK) != 0) { w.stage = "diagnostic_pipe"; return; }
    w.diagnosticFd = diagnostics[0];
    Socket diagnosticChild{diagnostics[1]};
    timeval timeout{5, 0};
    setsockopt(parent.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    NativeChildProcess_Fd channel{};
    channel.fdName = const_cast<char*>(kSocketName);
    channel.fd = child.value;
    NativeChildProcess_Fd diagnostic{};
    diagnostic.fdName = const_cast<char*>(kDiagnosticName);
    diagnostic.fd = diagnosticChild.value;
    channel.next = &diagnostic;
    NativeChildProcess_Args args{};
    args.entryParams = const_cast<char*>(w.standardFork ? "shared-buffer-p0-standard-fork" :
        w.preserveLowMappings ? "shared-buffer-p0-preserve-maps" : "shared-buffer-p0");
    args.fdList.head = &channel;
    if (w.systemStart) {
        { std::lock_guard<std::mutex> lock(systemExitMutex); systemExits.clear(); }
        w.observer.reset(new SystemObserver());
        if (!w.observer->start || w.observer->start == &OH_Ability_StartNativeChildProcess) {
            w.stage = "system_start_symbol"; w.launchCode = NCP_ERR_NOT_SUPPORTED; return;
        }
        NativeChildProcess_Options options{};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        w.launchCode = w.observer->start("libdirect_shared_buffer_probe.so:Main", args, options, &w.child);
    } else w.launchCode = Phone_StartViaDirectForkServer("libdirect_shared_buffer_probe.so:Main", args, &w.child);
    if (w.launchCode != NCP_NO_ERROR || w.child <= 0) {
        w.stage = w.systemStart ? "system_start_launch" : "fork_server_launch"; return;
    }
    if (w.systemStart && !w.observer->registered) { w.stage = "system_exit_observer"; return; }
    close(child.value);
    child.value = -1;
    if (!Send(parent.value, descriptor.packet, descriptor.fds)) {
        w.systemError = errno; w.stage = "buffer_send"; return;
    }
    descriptor.CloseFds();
    if (w.guestExport) {
        if (!Wait(w, parent.value, Kind::Buffer, descriptor)) return;
        if (descriptor.packet.width != 64 || descriptor.packet.height != 64 ||
            descriptor.packet.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888) {
            w.stage = "export_descriptor_config"; return;
        }
        w.backingFds = descriptor.packet.fdCount;
        w.originalFieldCount = descriptor.packet.originalFieldCount;
        if (!DecodeBuffer(descriptor, &buffer.value, &w.stage)) return;
        w.localRoundtrip = true;
        descriptor.CloseFds();
    }
    Message ready;
    if (!Wait(w, parent.value, Kind::Ready, ready)) return;
    if (!ready.fds.empty()) { w.stage = "ready_unexpected_fd"; return; }
    DirectBufferImportProbe sampler(true, 0, 1, true, true);
    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        Packet command{};
        command.kind = Kind::Render;
        command.frame = frame;
        if (!Send(parent.value, command)) { w.stage = "render_send"; return; }
        Message rendered;
        if (!Wait(w, parent.value, Kind::Frame, rendered)) return;
        if (rendered.packet.frame != frame || rendered.fds.size() > 1) { w.stage = "rendered_packet"; return; }
        int acquire = rendered.fds.empty() ? -1 : rendered.fds.front();
        const bool realAcquire = acquire >= 0;
        rendered.fds.clear();
        int release = -1;
        if (!sampler.Import(buffer.value, 64, 64) ||
            !sampler.SubmitSampleWithFences(buffer.value, 64, 64, frame, &acquire, &release)) {
            if (acquire >= 0) close(acquire);
            if (release >= 0) close(release);
            w.stage = sampler.Stage(); w.result = sampler.VkError(); return;
        }
        if (realAcquire) ++w.hostRealFenceImports;
        if (release >= 0) ++w.hostRealFenceExports;
        Packet returned{};
        returned.kind = Kind::Release;
        returned.frame = frame;
        const bool sent = Send(parent.value, returned,
            release >= 0 ? std::vector<int>{release} : std::vector<int>{});
        if (release >= 0) close(release);
        if (!sent) { w.stage = "release_send"; return; }
        // Pixel diagnostics read only nine GPU-produced uints, after returning
        // the real GPU release fence; no CPU mapping of the image is used.
        if (!sampler.FinishSample(frame)) { w.stage = sampler.Stage(); w.result = sampler.VkError(); return; }
        ++w.frames;
    }
    w.hostImports = sampler.ImportCount();
    w.hostReuses = sampler.ReuseCount();
    w.hostFenceImports = sampler.AcquireImportCount();
    w.hostFenceExports = sampler.ReleaseExportCount();
    Packet finish{};
    finish.kind = Kind::Finish;
    if (!Send(parent.value, finish)) { w.stage = "finish_send"; return; }
    Message completed;
    if (!Wait(w, parent.value, Kind::Complete, completed)) return;
    const bool ownership = w.guestExport ? w.childImports == 0 && w.childAllocations == 1 && w.childExports == 1 :
        w.childImports == 1 && w.childAllocations == 0 && w.childExports == 0;
    w.passed = w.frames == kFrameCount && ownership && w.hostImports == 1 &&
        w.hostReuses == kFrameCount - 1 && w.childFenceImports == kFrameCount &&
        w.childFenceExports == kFrameCount && w.hostFenceExports == kFrameCount &&
        w.hostFenceImports == w.hostRealFenceImports &&
        w.childRealFenceExports == w.hostRealFenceImports && w.hostRealFenceExports == w.childRealFenceImports;
    w.stage = w.passed ? "complete" : "counter_contract";
}

bool ObserveExit(Work& w) {
    if (w.systemStart) {
        std::lock_guard<std::mutex> lock(systemExitMutex);
        auto found = systemExits.find(w.child);
        if (found == systemExits.end()) return false;
        w.childSignal = found->second;
        w.exitObserved = true; // The system owns reaping; no waitpid/exit code claim.
        return true;
    }
    int status = 0;
    if (!Phone_QueryDirectForkChildExit(w.child, &status)) return false;
    w.reaped = w.exitObserved = true;
    w.childWaitStatus = status;
    if (WIFEXITED(status)) w.childExit = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) w.childSignal = WTERMSIG(status);
    return true;
}

void Execute(napi_env, void* data) {
    std::lock_guard<std::mutex> serialized(probeMutex);
    auto& w = *static_cast<Work*>(data);
    w.parentFdsBefore = CountFds();
    Run(w);
    if (w.child > 0) {
        if (!ObserveExit(w) && !w.passed) w.childKilledByProbe = kill(w.child, SIGKILL) == 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!w.exitObserved && std::chrono::steady_clock::now() < deadline && !ObserveExit(w))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        w.childKilledByProbe = w.childKilledByProbe && w.childSignal == SIGKILL;
        if (w.passed && (!w.exitObserved || (w.systemStart ? w.childSignal != 0 : w.childExit != 0))) {
            w.passed = false; w.stage = "child_exit";
        }
    }
    w.observer.reset();
    if (w.diagnosticFd >= 0) {
        size_t used = 0;
        ssize_t bytes;
        while (used < sizeof(w.childStderr) - 1 &&
               (bytes = read(w.diagnosticFd, w.childStderr + used, sizeof(w.childStderr) - 1 - used)) > 0)
            used += static_cast<size_t>(bytes);
        close(w.diagnosticFd);
        w.diagnosticFd = -1;
    }
    w.parentFdsAfter = CountFds();
}

void Complete(napi_env env, napi_status status, void* data) {
    auto* w = static_cast<Work*>(data);
    if (status != napi_ok) { w->passed = false; w->stage = "async_work"; }
    napi_value result;
    napi_create_object(env, &result);
    auto text = [&](const char* key, const char* value) {
        napi_value item; napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &item);
        napi_set_named_property(env, result, key, item);
    };
    auto number = [&](const char* key, int32_t value) {
        napi_value item; napi_create_int32(env, value, &item);
        napi_set_named_property(env, result, key, item);
    };
    auto boolean = [&](const char* key, bool value) {
        napi_value item; napi_get_boolean(env, value, &item);
        napi_set_named_property(env, result, key, item);
    };
    text("gate", w->guestExport ? "P0-PHONE-GUEST-EXPORT" : "P0-PHONE-SHARED-NATIVEBUFFER");
    text("status", w->passed ? "PASS" : "FAIL");
    text("stage", w->stage);
    text("lastChildStage", w->lastChildStage);
    text("loaderPath", w->loaderPath);
    text("deviceName", w->deviceName);
    text("childStderr", w->childStderr);
    number("parentPid", getpid()); number("pid", w->child); number("forkServerPid", w->server);
    number("launchCode", w->launchCode); number("childExitCode", w->childExit);
    number("childWaitStatus", w->childWaitStatus); number("childSignal", w->childSignal);
    boolean("childReaped", w->reaped); boolean("localParcelRoundtrip", w->localRoundtrip);
    boolean("childExitObserved", w->exitObserved);
    boolean("preserveLowMappings", w->preserveLowMappings);
    boolean("standardFork", w->standardFork);
    boolean("systemStart", w->systemStart);
    boolean("guestExport", w->guestExport);
    boolean("childKilledByProbe", w->childKilledByProbe);
    boolean("childDeviceCreated", w->childDeviceCreated);
    number("vkResult", w->result); number("parentFdBefore", w->parentFdsBefore); number("parentFdAfter", w->parentFdsAfter);
    number("systemError", w->systemError);
    number("backingFds", w->backingFds); number("originalFieldCount", w->originalFieldCount);
    number("framesVerified", w->frames); number("childImageImports", w->childImports);
    number("childImageAllocations", w->childAllocations); number("childNativeBufferExports", w->childExports);
    number("childRealReleaseFenceImports", w->childRealFenceImports);
    number("childRealRenderFenceExports", w->childRealFenceExports);
    number("hostRealRenderFenceImports", w->hostRealFenceImports);
    number("hostRealReleaseFenceExports", w->hostRealFenceExports);
    number("childReleaseFenceImports", w->childFenceImports); number("childRenderFenceExports", w->childFenceExports);
    number("hostImageImports", w->hostImports); number("hostImageReuses", w->hostReuses);
    number("hostRenderFenceImports", w->hostFenceImports); number("hostReleaseFenceExports", w->hostFenceExports);
    number("diagnosticCpuReadBytes", w->frames * 9 * 4);
    number("imageCpuReadBytes", 0); number("imageCpuUploadBytes", 0); number("imageGpuCopyCount", 0);
    napi_resolve_deferred(env, w->deferred, result);
    napi_delete_async_work(env, w->async);
    delete w;
}
}

napi_value RunPhoneSharedBufferProbe(napi_env env, napi_callback_info info) {
    auto* work = new Work();
    napi_value arguments[4];
    size_t count = 4;
    if (napi_get_cb_info(env, info, &count, arguments, nullptr, nullptr) == napi_ok) {
        if (count) napi_get_value_bool(env, arguments[0], &work->preserveLowMappings);
        if (count > 1) napi_get_value_bool(env, arguments[1], &work->standardFork);
        if (count > 2) napi_get_value_bool(env, arguments[2], &work->systemStart);
        if (count > 3) napi_get_value_bool(env, arguments[3], &work->guestExport);
    }
    if (work->systemStart) work->preserveLowMappings = work->standardFork = false;
    if (work->standardFork) work->preserveLowMappings = true;
    napi_value promise, name;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work; napi_throw_error(env, nullptr, "shared buffer promise"); return nullptr;
    }
    napi_create_string_utf8(env, "WineHuaPhoneSharedBufferP0", NAPI_AUTO_LENGTH, &name);
    if (napi_create_async_work(env, nullptr, name, Execute, Complete, work, &work->async) != napi_ok ||
        napi_queue_async_work(env, work->async) != napi_ok) {
        if (work->async) napi_delete_async_work(env, work->async);
        delete work; napi_throw_error(env, nullptr, "shared buffer async work"); return nullptr;
    }
    return promise;
}
}
