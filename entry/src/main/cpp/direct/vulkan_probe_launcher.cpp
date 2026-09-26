#include "vulkan_probe_launcher.h"
#include "vulkan_probe_protocol.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_D0_MAIN"
#include <hilog/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <new>

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

void ExecuteProbe(napi_env, void* data)
{
    auto& work = *static_cast<ProbeWork*>(data);
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        SetFailure(work, "socketpair");
        return;
    }
    struct stat outputIdentity{};
    const bool haveOutputIdentity = fstat(sockets[1], &outputIdentity) == 0;

    NativeChildProcess_Fd output{};
    output.fdName = const_cast<char*>(kVulkanProbeFdName);
    output.fd = sockets[1];
    NativeChildProcess_Args args{};
    args.entryParams = const_cast<char*>("d0");
    args.fdList.head = &output;
    NativeChildProcess_Options options{};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t childPid = -1;
    work.ncpStatus = OH_Ability_StartNativeChildProcess(
        "libdirect_vulkan_probe.so:Main", args, options, &childPid);
    if (work.ncpStatus != NCP_NO_ERROR || childPid <= 0) {
        close(sockets[0]);
        close(sockets[1]);
        SetFailure(work, "start_ncp");
        return;
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
    // Read the fixed packet rather than waiting for EOF, which may be held by the NCP runtime.
    work.result.pid = childPid;
    auto* bytes = reinterpret_cast<uint8_t*>(&work.result);
    size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (received < sizeof(work.result)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            SetFailure(work, "result_timeout");
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{};
        pfd.fd = sockets[0];
        pfd.events = POLLIN;
        const int polled = poll(&pfd, 1, static_cast<int>(remaining));
        if (polled < 0 && errno == EINTR) continue;
        if (polled < 0) {
            SetFailure(work, "result_poll");
            break;
        }
        if (!polled) {
            SetFailure(work, "result_timeout");
            break;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP))) {
            SetFailure(work, "result_socket");
            break;
        }
        const ssize_t count = read(sockets[0], bytes + received, sizeof(work.result) - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            SetFailure(work, "result_eof");
            break;
        }
        received += static_cast<size_t>(count);
    }
    close(sockets[0]);
    if (received == sizeof(work.result) &&
        (work.result.magic != kVulkanProbeMagic ||
         work.result.version != kVulkanProbeVersion ||
         work.result.size != sizeof(work.result) ||
         work.result.pid != childPid))
        SetFailure(work, "result_protocol");
}

void ExecuteCreateProbe(napi_env, void* data)
{
    auto& work = *static_cast<ProbeWork*>(data);
    work.launchMode = "CreateNativeChildProcess";
    {
        std::lock_guard<std::mutex> lock(g_createMutex);
        if (g_createPending) {
            SetFailure(work, "create_pending");
            return;
        }
        g_createPending = true;
        g_createWork = &work;
    }
    work.ncpStatus = OH_Ability_CreateNativeChildProcess(
        "libdirect_vulkan_probe.so", OnCreateProbeStarted);
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
    SetInt(env, result, "parentFdCount", CountOpenFds());
    SetInt(env, result, "parentRssKiB", ReadRssKiB());
    SetInt(env, result, "vkResult", work->result.vkResult);
    SetUint(env, result, "loaderVersion", work->result.loaderVersion);
    SetUint(env, result, "requestedApiVersion", work->result.requestedApiVersion);
    SetUint(env, result, "instanceExtensionCount", work->result.instanceExtensionCount);
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

napi_value RunVulkanProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) ProbeWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate D0 probe work");
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
                               work, &work->work) != napi_ok ||
        napi_queue_async_work(env, work->work) != napi_ok) {
        if (work->work) napi_delete_async_work(env, work->work);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue D0 probe");
        return nullptr;
    }
    return promise;
}

napi_value RunVulkanCreateProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) ProbeWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate D0 Create probe work");
        return nullptr;
    }
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

} // namespace winehua::direct
