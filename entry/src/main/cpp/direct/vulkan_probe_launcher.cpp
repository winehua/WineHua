#include "vulkan_probe_launcher.h"
#include "vulkan_probe_protocol.h"
#include "phone_adapter/phone_adapter.h"
#include "phone_adapter/phone_process.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <dlfcn.h>
#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_D0_MAIN"
#include <hilog/log.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace winehua::direct {
namespace {

struct ProbeWork {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    VulkanProbeResult result{};
    int32_t ncpStatus = -1;
    int32_t callbackStatus = -1;
    const char* launchMode = "StartNativeChildProcess";
    OHIPCRemoteProxy* proxy = nullptr;
    bool callbackReceived = false;
    bool systemCreate = false;
    bool prepared = false;
    bool forkServer = false;
    int32_t forkServerPid = -1;
    int32_t childExitCode = -1;
    int32_t childWaitStatus = -1;
    bool childReaped = false;
    int resultFd = -1;
    int stageFd = -1;
};

std::mutex g_createMutex;
std::condition_variable g_createCondition;
ProbeWork* g_createWork = nullptr;
bool g_createPending = false;

int CountOpenFds()
{
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (const dirent* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    closedir(directory);
    return count - 1; // exclude the descriptor opened by opendir itself
}

int ReadRssKiB()
{
    FILE* statm = std::fopen("/proc/self/statm", "r");
    if (!statm) return -1;
    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    const int scanned = std::fscanf(statm, "%lu %lu", &totalPages, &residentPages);
    std::fclose(statm);
    return scanned == 2 ? static_cast<int>(residentPages * sysconf(_SC_PAGESIZE) / 1024) : -1;
}

void OnCreateProbeStarted(int errorCode, OHIPCRemoteProxy* proxy)
{
    {
        std::lock_guard<std::mutex> lock(g_createMutex);
        if (g_createWork) {
            g_createWork->callbackStatus = errorCode;
            g_createWork->proxy = proxy;
            g_createWork->callbackReceived = true;
            g_createCondition.notify_all();
            return;
        }
        // A timed-out callback must be consumed before another launch is accepted.
        g_createPending = false;
    }
    if (proxy) OH_IPCRemoteProxy_Destroy(proxy);
}

void SetString(napi_env env, napi_value object, const char* key, const char* value)
{
    napi_value item;
    napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &item);
    napi_set_named_property(env, object, key, item);
}

void SetInt(napi_env env, napi_value object, const char* key, int32_t value)
{
    napi_value item;
    napi_create_int32(env, value, &item);
    napi_set_named_property(env, object, key, item);
}

void SetUint(napi_env env, napi_value object, const char* key, uint32_t value)
{
    napi_value item;
    napi_create_uint32(env, value, &item);
    napi_set_named_property(env, object, key, item);
}

void SetBool(napi_env env, napi_value object, const char* key, bool value)
{
    napi_value item;
    napi_get_boolean(env, value, &item);
    napi_set_named_property(env, object, key, item);
}

void SetFailure(ProbeWork& work, const char* stage)
{
    work.result.status = -1;
    std::snprintf(work.result.stage, sizeof(work.result.stage), "%s", stage);
}

bool StartProbe(ProbeWork& work)
{
    work.prepared = true;
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        SetFailure(work, "socketpair");
        return false;
    }
    int stages[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stages) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        SetFailure(work, "stage_socketpair");
        return false;
    }
    struct stat outputIdentity{};
    const bool haveOutputIdentity = fstat(sockets[1], &outputIdentity) == 0;
    struct stat stageIdentity{};
    const bool haveStageIdentity = fstat(stages[1], &stageIdentity) == 0;

    NativeChildProcess_Fd output{};
    output.fdName = const_cast<char*>(kVulkanProbeFdName);
    output.fd = sockets[1];
    NativeChildProcess_Fd stage{};
    stage.fdName = const_cast<char*>(kVulkanProbeStageFdName);
    stage.fd = stages[1];
    output.next = &stage;
    NativeChildProcess_Args args{};
    args.entryParams = const_cast<char*>("d0");
    args.fdList.head = &output;
    NativeChildProcess_Options options{};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t childPid = -1;
    work.ncpStatus = work.forkServer ?
        Phone_StartViaDirectForkServer("libdirect_vulkan_probe.so:Main", args, &childPid) :
        OH_Ability_StartNativeChildProcess("libdirect_vulkan_probe.so:Main", args, options, &childPid);
    if (work.ncpStatus != NCP_NO_ERROR || childPid <= 0) {
        char stagesSeen[64];
        const ssize_t stageCount = recv(stages[0], stagesSeen, sizeof(stagesSeen), MSG_DONTWAIT);
        const char lastStage = stageCount > 0 ? stagesSeen[stageCount - 1] : 0;
        close(sockets[0]);
        close(sockets[1]);
        close(stages[0]);
        close(stages[1]);
        SetFailure(work, "start_ncp");
        if (lastStage >= '1' && lastStage <= '5')
            std::snprintf(work.result.stage, sizeof(work.result.stage), "start_ncp_after_stage_%c", lastStage);
        if (work.forkServer && childPid > 0) {
            work.result.pid = childPid;
            int status = 0;
            for (int attempt = 0; attempt < 100; ++attempt) {
                if ((work.childReaped = Phone_QueryDirectForkChildExit(childPid, &status))) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (work.childReaped && WIFEXITED(status)) work.childExitCode = WEXITSTATUS(status);
            if (work.childReaped) work.childWaitStatus = status;
        }
        return false;
    }
    // The device's Start API duplicates the descriptor for the child but leaves
    // the caller's original open. Close only if it is still the same socket.
    struct stat currentIdentity{};
    const bool outputFdRetained = haveOutputIdentity &&
        fstat(sockets[1], &currentIdentity) == 0 &&
        currentIdentity.st_dev == outputIdentity.st_dev &&
        currentIdentity.st_ino == outputIdentity.st_ino;
    OH_LOG_INFO(LOG_APP, "[DIRECT-D0] Start output fd retained=%{public}d fd=%{public}d",
                outputFdRetained ? 1 : 0, sockets[1]);
    if (outputFdRetained)
        close(sockets[1]);
    const bool stageFdRetained = haveStageIdentity &&
        fstat(stages[1], &currentIdentity) == 0 &&
        currentIdentity.st_dev == stageIdentity.st_dev &&
        currentIdentity.st_ino == stageIdentity.st_ino;
    if (stageFdRetained) close(stages[1]);
    work.result.pid = childPid;
    work.resultFd = sockets[0];
    work.stageFd = stages[0];
    return true;
}

void ExecuteProbe(napi_env, void* data)
{
    auto& work = *static_cast<ProbeWork*>(data);
    if (!work.prepared) StartProbe(work);
    if (work.resultFd < 0) return;
    const int childPid = work.result.pid;
    int resultFd = work.resultFd;
    int stageFd = work.stageFd;
    // Read the fixed packet rather than waiting for EOF, which may be held by the NCP runtime.
    auto* bytes = reinterpret_cast<uint8_t*>(&work.result);
    size_t received = 0;
    char lastStage = 0;
    const char* transportFailure = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (received < sizeof(work.result)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd[2] = {{resultFd, POLLIN, 0}, {stageFd, POLLIN, 0}};
        const int polled = poll(pfd, 2, static_cast<int>(remaining));
        if (polled < 0 && errno == EINTR) continue;
        if (polled < 0) {
            transportFailure = "result_poll";
            break;
        }
        if (!polled) break;
        if (stageFd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            char reported[16];
            const ssize_t count = read(stageFd, reported, sizeof(reported));
            if (count > 0) lastStage = reported[count - 1];
            else if (count == 0) {
                close(stageFd);
                stageFd = -1;
            }
        }
        if (!pfd[0].revents) continue;
        if (!(pfd[0].revents & (POLLIN | POLLHUP))) {
            transportFailure = "result_socket";
            break;
        }
        const ssize_t count = read(resultFd, bytes + received, sizeof(work.result) - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            transportFailure = "result_eof";
            break;
        }
        received += static_cast<size_t>(count);
    }
    close(resultFd);
    if (stageFd >= 0) close(stageFd);
    work.resultFd = work.stageFd = -1;
    if (received != sizeof(work.result)) {
        const char* stage = "result_timeout_before_main";
        switch (lastStage) {
            case 'M': stage = "timeout_after_main"; break;
            case 'L': stage = "timeout_in_vulkan_dlopen"; break;
            case 'l': stage = "timeout_after_vulkan_dlopen"; break;
            case 'E': stage = "timeout_in_extension_query"; break;
            case 'e': stage = "timeout_after_extension_query"; break;
            case 'I': stage = "timeout_in_vkCreateInstance"; break;
            case 'i': stage = "timeout_after_vkCreateInstance"; break;
            case 'h': stage = "timeout_after_instance_log"; break;
            case 'f': stage = "timeout_in_instance_failure_cleanup"; break;
            case 'J': stage = "timeout_resolving_instance_functions"; break;
            case 'j': stage = "timeout_after_instance_functions"; break;
            case 'P': stage = "timeout_enumerating_devices_count"; break;
            case 'p': stage = "timeout_after_devices_count"; break;
            case 'R': stage = "timeout_enumerating_devices_list"; break;
            case 'r': stage = "timeout_after_devices_list"; break;
            case 'G': stage = "timeout_selecting_graphics_device"; break;
            case 'g': stage = "timeout_after_graphics_selection"; break;
            case 'D': stage = "timeout_in_vkCreateDevice"; break;
            case 'd': stage = "timeout_after_vkCreateDevice"; break;
            case 'C': stage = "timeout_after_probe_cleanup"; break;
            case 'W': stage = "timeout_writing_result"; break;
            case 'w': stage = "timeout_after_result_write"; break;
        }
        SetFailure(work, transportFailure ? transportFailure : stage);
    }
    if (received == sizeof(work.result) &&
        (work.result.magic != kVulkanProbeMagic ||
         work.result.version != kVulkanProbeVersion ||
         work.result.size != sizeof(work.result) ||
         work.result.pid != childPid))
        SetFailure(work, "result_protocol");
    if (work.forkServer) {
        if (received != sizeof(work.result)) kill(childPid, SIGKILL);
        int waitStatus = 0;
        const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < reapDeadline &&
               !(work.childReaped = Phone_QueryDirectForkChildExit(childPid, &waitStatus)))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (work.childReaped && WIFEXITED(waitStatus)) work.childExitCode = WEXITSTATUS(waitStatus);
        if (work.childReaped) work.childWaitStatus = waitStatus;
        if (work.result.status == 0 && (!work.childReaped || work.childExitCode != 0))
            SetFailure(work, "fork_server_child_exit");
    }
}

void ExecuteCreateProbe(napi_env, void* data)
{
    auto& work = *static_cast<ProbeWork*>(data);
    work.launchMode = work.systemCreate ?
        "CreateNativeChildProcess(system)" : "CreateNativeChildProcess";
    {
        std::lock_guard<std::mutex> lock(g_createMutex);
        if (g_createPending) {
            SetFailure(work, "create_pending");
            return;
        }
        g_createPending = true;
        g_createWork = &work;
    }
    if (work.systemCreate) {
        // A phone normally interposes Create with its fork adapter. This
        // diagnostic calls the OS implementation explicitly so its result
        // cannot be mistaken for the adapter's dummy proxy.
        static void* systemLibrary = dlopen("libchild_process.so", RTLD_NOW | RTLD_LOCAL);
        auto systemCreate = systemLibrary ?
            reinterpret_cast<decltype(&OH_Ability_CreateNativeChildProcess)>(
                dlsym(systemLibrary, "OH_Ability_CreateNativeChildProcess")) : nullptr;
        if (!systemCreate || systemCreate == &OH_Ability_CreateNativeChildProcess) {
            std::lock_guard<std::mutex> lock(g_createMutex);
            g_createWork = nullptr;
            g_createPending = false;
            work.ncpStatus = NCP_ERR_NOT_SUPPORTED;
            SetFailure(work, "system_create_symbol");
            return;
        }
        work.ncpStatus = systemCreate("libdirect_vulkan_probe.so", OnCreateProbeStarted);
    } else {
        work.ncpStatus = OH_Ability_CreateNativeChildProcess(
            "libdirect_vulkan_probe.so", OnCreateProbeStarted);
    }
    if (work.ncpStatus != NCP_NO_ERROR) {
        std::lock_guard<std::mutex> lock(g_createMutex);
        g_createWork = nullptr;
        g_createPending = false;
        SetFailure(work, "create_ncp");
        return;
    }
    {
        std::unique_lock<std::mutex> lock(g_createMutex);
        if (!g_createCondition.wait_for(lock, std::chrono::seconds(15),
                                        [&work] { return work.callbackReceived; })) {
            g_createWork = nullptr;
            SetFailure(work, "create_callback_timeout");
            return;
        }
        g_createWork = nullptr;
        g_createPending = false;
    }
    if (work.callbackStatus != NCP_NO_ERROR || !work.proxy) {
        if (work.proxy) OH_IPCRemoteProxy_Destroy(work.proxy);
        SetFailure(work, "create_callback");
        return;
    }
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    if (!request || !reply ||
        OH_IPCParcel_WriteInt32(request, kVulkanProbeVersion) != OH_IPC_SUCCESS) {
        SetFailure(work, "create_request_alloc");
    } else {
        const int ipcStatus = OH_IPCRemoteProxy_SendRequest(
            work.proxy, kVulkanProbeReadRequest, request, reply, nullptr);
        const auto* bytes = ipcStatus == OH_IPC_SUCCESS
            ? OH_IPCParcel_ReadBuffer(reply, sizeof(work.result)) : nullptr;
        if (!bytes) {
            SetFailure(work, "create_result_ipc");
        } else {
            std::memcpy(&work.result, bytes, sizeof(work.result));
            if (work.result.magic != kVulkanProbeMagic ||
                work.result.version != kVulkanProbeVersion ||
                work.result.size != sizeof(work.result) || work.result.pid <= 0)
                SetFailure(work, "create_result_protocol");
        }
    }
    if (request) OH_IPCParcel_Destroy(request);
    if (reply) OH_IPCParcel_Destroy(reply);
    // Release the child only after the result reply has been copied locally.
    request = OH_IPCParcel_Create();
    reply = OH_IPCParcel_Create();
    if (request && reply &&
        OH_IPCParcel_WriteInt32(request, kVulkanProbeVersion) == OH_IPC_SUCCESS)
        OH_IPCRemoteProxy_SendRequest(work.proxy, kVulkanProbeFinishRequest,
                                      request, reply, nullptr);
    if (request) OH_IPCParcel_Destroy(request);
    if (reply) OH_IPCParcel_Destroy(reply);
    OH_IPCRemoteProxy_Destroy(work.proxy);
}

void ExecuteInlineProbe(napi_env, void* data)
{
    auto& work = *static_cast<ProbeWork*>(data);
    work.launchMode = "InProcess";
    work.result.pid = getpid();
    void* library = dlopen("libdirect_vulkan_probe.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        SetFailure(work, "inline_dlopen");
        return;
    }
    using RunFn = void (*)(VulkanProbeResult*);
    auto run = reinterpret_cast<RunFn>(dlsym(library, "DirectVulkanProbe_RunInline"));
    if (!run) {
        SetFailure(work, "inline_symbol");
        return;
    }
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        VulkanProbeResult result{};
        bool done = false;
    };
    auto state = std::make_shared<State>();
    std::thread([state, run] {
        run(&state->result);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->condition.notify_one();
    }).detach();
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, std::chrono::seconds(15),
                                   [&] { return state->done; })) {
        SetFailure(work, "inline_timeout");
        return;
    }
    work.result = state->result;
}

void CompleteProbe(napi_env env, napi_status status, void* data)
{
    auto* work = static_cast<ProbeWork*>(data);
    if (status != napi_ok) SetFailure(*work, "async_work");
    napi_value result;
    napi_create_object(env, &result);
    SetString(env, result, "gate", "D0");
    SetString(env, result, "launchMode", work->launchMode);
    SetString(env, result, "status", work->result.status == 0 ? "PASS" : "FAIL");
    SetString(env, result, "stage", work->result.stage);
    SetString(env, result, "loaderPath", work->result.loaderPath);
    SetString(env, result, "deviceName", work->result.deviceName);
    SetInt(env, result, "pid", work->result.pid);
    SetInt(env, result, "ncpStatus", work->ncpStatus);
    SetInt(env, result, "callbackStatus", work->callbackStatus);
    if (work->forkServer) {
        SetInt(env, result, "forkServerPid", work->forkServerPid);
        SetInt(env, result, "childExitCode", work->childExitCode);
        SetInt(env, result, "childWaitStatus", work->childWaitStatus);
        SetBool(env, result, "childReaped", work->childReaped);
    }
    SetInt(env, result, "parentFdCount", CountOpenFds());
    SetInt(env, result, "parentRssKiB", ReadRssKiB());
    SetInt(env, result, "vkResult", work->result.vkResult);
    SetUint(env, result, "loaderVersion", work->result.loaderVersion);
    SetUint(env, result, "requestedApiVersion", work->result.requestedApiVersion);
    SetUint(env, result, "instanceExtensionCount", work->result.instanceExtensionCount);
    SetUint(env, result, "deviceExtensionCount", work->result.deviceExtensionCount);
    SetUint(env, result, "nativeCapabilities", work->result.nativeCapabilities);
    napi_value externalImages;
    napi_create_array(env, &externalImages);
    for (uint32_t i = 0; i < work->result.externalImageCount && i < kExternalImageProbeCount; ++i) {
        const auto& audit = work->result.externalImages[i];
        napi_value item;
        napi_create_object(env, &item);
        SetUint(env, item, "handleType", audit.handleType);
        SetUint(env, item, "format", audit.format);
        SetUint(env, item, "tiling", audit.tiling);
        SetUint(env, item, "usage", audit.usage);
        SetInt(env, item, "result", audit.result);
        SetUint(env, item, "features", audit.features);
        SetUint(env, item, "compatibleHandleTypes", audit.compatibleHandleTypes);
        SetUint(env, item, "exportFromImportedHandleTypes", audit.exportFromImportedHandleTypes);
        napi_set_element(env, externalImages, i, item);
    }
    napi_set_named_property(env, result, "externalImages", externalImages);
    SetBool(env, result, "opaqueFdBufferQueried", work->result.opaqueFdBufferQueried != 0);
    SetUint(env, result, "opaqueFdBufferFeatures", work->result.opaqueFdBufferFeatures);
    SetUint(env, result, "apiVersion", work->result.apiVersion);
    SetString(env, result, "icdEnvironment", work->result.icdEnvironment);
    SetBool(env, result, "pixelCheck", work->result.pixelCheck != 0);
    napi_value elapsed;
    napi_create_int64(env, static_cast<int64_t>(work->result.elapsedMs), &elapsed);
    napi_set_named_property(env, result, "elapsedMs", elapsed);
    napi_resolve_deferred(env, work->deferred, result);
    napi_delete_async_work(env, work->work);
    delete work;
}

} // namespace

napi_value RunVulkanProbe(napi_env env, napi_callback_info info)
{
    auto* work = new (std::nothrow) ProbeWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate D0 probe work");
        return nullptr;
    }
    size_t argc = 1;
    napi_value argument;
    bool earlyPhoneFork = false;
    napi_get_cb_info(env, info, &argc, &argument, nullptr, nullptr);
    if (argc && napi_get_value_bool(env, argument, &earlyPhoneFork) != napi_ok) {
        delete work;
        napi_throw_type_error(env, nullptr, "earlyPhoneFork must be a boolean");
        return nullptr;
    }
    if (earlyPhoneFork && !PhoneAdapter_IsPhoneMode()) {
        delete work;
        napi_throw_error(env, nullptr, "early fork diagnostic requires phone mode");
        return nullptr;
    }
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        napi_throw_error(env, nullptr, "failed to create D0 probe promise");
        return nullptr;
    }
    napi_value resourceName;
    napi_create_string_utf8(env, "WineHuaDirectD0", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_async_work(env, nullptr, resourceName, ExecuteProbe, CompleteProbe,
                               work, &work->work) != napi_ok) {
        if (work->work) napi_delete_async_work(env, work->work);
        delete work;
        napi_throw_error(env, nullptr, "failed to create D0 probe work");
        return nullptr;
    }
    if (earlyPhoneFork) {
        work->launchMode = "StartNativeChildProcess(early-phone-fork)";
        // Launch synchronously from Ability.onCreate, before ArkUI loads the
        // diagnostic page. Only result polling is sent to the async worker.
        StartProbe(*work);
    }
    if (napi_queue_async_work(env, work->work) != napi_ok) {
        if (work->resultFd >= 0) close(work->resultFd);
        if (work->stageFd >= 0) close(work->stageFd);
        if (earlyPhoneFork && work->result.pid > 0) kill(work->result.pid, SIGKILL);
        napi_delete_async_work(env, work->work);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue D0 probe");
        return nullptr;
    }
    return promise;
}

napi_value RunVulkanInlineProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) ProbeWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate inline D0 probe work");
        return nullptr;
    }
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        napi_throw_error(env, nullptr, "failed to create inline D0 probe promise");
        return nullptr;
    }
    napi_value resourceName;
    napi_create_string_utf8(env, "WineHuaDirectD0Inline", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_async_work(env, nullptr, resourceName, ExecuteInlineProbe, CompleteProbe,
                               work, &work->work) != napi_ok ||
        napi_queue_async_work(env, work->work) != napi_ok) {
        if (work->work) napi_delete_async_work(env, work->work);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue inline D0 probe");
        return nullptr;
    }
    return promise;
}

napi_value PreparePhoneDirectForkServer(napi_env env, napi_callback_info)
{
    const int32_t status = PhoneAdapter_IsPhoneMode() ?
        Phone_PrepareDirectForkServer() : NCP_ERR_NOT_SUPPORTED;
    napi_value result;
    napi_create_int32(env, status, &result);
    return result;
}

napi_value RunVulkanForkServerProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) ProbeWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate fork server probe");
        return nullptr;
    }
    work->forkServer = true;
    work->forkServerPid = Phone_GetDirectForkServerPid();
    work->launchMode = "StartNativeChildProcess(early-phone-fork-server)";
    napi_value promise, resourceName;
    napi_create_string_utf8(env, "WineHuaDirectD0ForkServer", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok ||
        napi_create_async_work(env, nullptr, resourceName, ExecuteProbe, CompleteProbe,
                               work, &work->work) != napi_ok ||
        napi_queue_async_work(env, work->work) != napi_ok) {
        if (work->work) napi_delete_async_work(env, work->work);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue fork server probe");
        return nullptr;
    }
    return promise;
}

napi_value QueueCreateProbe(napi_env env, bool systemCreate)
{
    auto* work = new (std::nothrow) ProbeWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate D0 Create probe work");
        return nullptr;
    }
    work->systemCreate = systemCreate;
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        napi_throw_error(env, nullptr, "failed to create D0 Create probe promise");
        return nullptr;
    }
    napi_value resourceName;
    napi_create_string_utf8(env, "WineHuaDirectD0Create", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_async_work(env, nullptr, resourceName, ExecuteCreateProbe, CompleteProbe,
                               work, &work->work) != napi_ok ||
        napi_queue_async_work(env, work->work) != napi_ok) {
        if (work->work) napi_delete_async_work(env, work->work);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue D0 Create probe");
        return nullptr;
    }
    return promise;
}

napi_value RunVulkanCreateProbe(napi_env env, napi_callback_info)
{
    return QueueCreateProbe(env, false);
}

napi_value RunVulkanSystemCreateProbe(napi_env env, napi_callback_info)
{
    return QueueCreateProbe(env, true);
}

} // namespace winehua::direct
