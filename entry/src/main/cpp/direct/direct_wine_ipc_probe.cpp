#include "direct_wine_ipc_probe.h"
#include "proc/wine_child_ipc.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>

namespace winehua::direct {
namespace {

struct Work {
    napi_async_work asyncWork = nullptr;
    napi_deferred deferred = nullptr;
    OHIPCRemoteProxy* proxy = nullptr;
    int32_t launchCode = -1;
    int32_t callbackCode = -1;
    int32_t replyPid = -1;
    int32_t ipcCode = -1;
    bool callbackReceived = false;
    bool parentFdsOpen = false;
    std::mutex deathMutex;
    std::condition_variable deathCondition;
    bool deathReceived = false;
    wineipc::ProbeResult result{-1, -1, 0, 0};
    const char* stage = "pending";
};

std::mutex g_mutex;
std::condition_variable g_callback;
Work* g_active = nullptr;
bool g_pending = false;

void OnStarted(int32_t code, OHIPCRemoteProxy* proxy)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) {
        g_active->callbackCode = code;
        g_active->proxy = proxy;
        g_active->callbackReceived = true;
        g_callback.notify_one();
    } else {
        if (proxy) OH_IPCRemoteProxy_Destroy(proxy);
        g_pending = false;
    }
}

void OnChildDeath(void* data)
{
    auto* work = static_cast<Work*>(data);
    {
        std::lock_guard<std::mutex> lock(work->deathMutex);
        work->deathReceived = true;
    }
    work->deathCondition.notify_one();
}

void Execute(napi_env, void* data)
{
    auto& work = *static_cast<Work*>(data);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending) {
            work.stage = "create_pending";
            return;
        }
        g_pending = true;
        g_active = &work;
    }
    work.launchCode = OH_Ability_CreateNativeChildProcess("libwine_child.so", OnStarted);
    if (work.launchCode != NCP_NO_ERROR) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active = nullptr;
        g_pending = false;
        work.stage = "create_ncp";
        return;
    }
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_callback.wait_for(lock, std::chrono::seconds(15),
                                 [&work] { return work.callbackReceived; })) {
            g_active = nullptr; // late callback destroys its proxy
            work.stage = "create_callback_timeout";
            return;
        }
        g_active = nullptr;
        g_pending = false;
    }
    if (work.callbackCode != NCP_NO_ERROR || !work.proxy) {
        work.stage = "create_callback";
        if (work.proxy) OH_IPCRemoteProxy_Destroy(work.proxy);
        return;
    }

    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    OHIPCDeathRecipient* deathRecipient = nullptr;
    bool deathRegistered = false;
    OHIPCParcel* request = nullptr;
    OHIPCParcel* reply = nullptr;
    do {
        deathRecipient = OH_IPCDeathRecipient_Create(OnChildDeath, nullptr, &work);
        if (!deathRecipient ||
            OH_IPCRemoteProxy_AddDeathRecipient(work.proxy, deathRecipient) != OH_IPC_SUCCESS) {
            work.stage = "add_death_recipient";
            break;
        }
        deathRegistered = true;
        if (pipe(input) || pipe(output)) {
            work.stage = "pipe";
            break;
        }
        const uint32_t token = wineipc::kProbeToken;
        if (write(input[1], &token, sizeof(token)) != sizeof(token)) {
            work.stage = "write_token";
            break;
        }
        request = OH_IPCParcel_Create();
        reply = OH_IPCParcel_Create();
        if (!request || !reply ||
            OH_IPCParcel_WriteInt32(request, wineipc::kVersion) != OH_IPC_SUCCESS ||
            OH_IPCParcel_WriteString(request, wineipc::kProbeParams) != OH_IPC_SUCCESS ||
            OH_IPCParcel_WriteInt32(request, 2) != OH_IPC_SUCCESS ||
            OH_IPCParcel_WriteString(request, "probe_input") != OH_IPC_SUCCESS ||
            OH_IPCParcel_WriteFileDescriptor(request, input[0]) != OH_IPC_SUCCESS ||
            OH_IPCParcel_WriteString(request, "probe_output") != OH_IPC_SUCCESS ||
            OH_IPCParcel_WriteFileDescriptor(request, output[1]) != OH_IPC_SUCCESS) {
            work.stage = "write_parcel";
            break;
        }
        work.ipcCode = OH_IPCRemoteProxy_SendRequest(
            work.proxy, wineipc::kBootstrap, request, reply, nullptr);
        work.parentFdsOpen = fcntl(input[0], F_GETFD) >= 0 &&
                             fcntl(output[1], F_GETFD) >= 0;
        if (work.ipcCode != OH_IPC_SUCCESS ||
            OH_IPCParcel_ReadInt32(reply, &work.replyPid) != OH_IPC_SUCCESS ||
            work.replyPid <= 0) {
            work.stage = "bootstrap_ipc";
            break;
        }
        close(input[0]); input[0] = -1;
        close(output[1]); output[1] = -1;
        pollfd pfd{output[0], POLLIN, 0};
        if (poll(&pfd, 1, 5000) <= 0 || !(pfd.revents & (POLLIN | POLLHUP))) {
            work.stage = "result_timeout";
            break;
        }
        if (read(output[0], &work.result, sizeof(work.result)) != sizeof(work.result)) {
            work.stage = "result_read";
            break;
        }
        if (work.result.pid != work.replyPid || work.result.status != 0 ||
            work.result.token != wineipc::kProbeToken || work.result.fdCount != 2 ||
            !work.parentFdsOpen) {
            work.stage = "result_mismatch";
            break;
        }
        {
            std::unique_lock<std::mutex> lock(work.deathMutex);
            if (!work.deathCondition.wait_for(lock, std::chrono::seconds(5),
                                              [&work] { return work.deathReceived; })) {
                work.stage = "death_timeout";
                break;
            }
        }
        work.stage = "complete";
    } while (false);
    if (request) OH_IPCParcel_Destroy(request);
    if (reply) OH_IPCParcel_Destroy(reply);
    for (int fd : input) if (fd >= 0) close(fd);
    for (int fd : output) if (fd >= 0) close(fd);
    if (deathRecipient) {
        if (deathRegistered) OH_IPCRemoteProxy_RemoveDeathRecipient(work.proxy, deathRecipient);
        OH_IPCDeathRecipient_Destroy(deathRecipient);
    }
    OH_IPCRemoteProxy_Destroy(work.proxy);
}

void SetInt(napi_env env, napi_value object, const char* key, int32_t value)
{
    napi_value field;
    napi_create_int32(env, value, &field);
    napi_set_named_property(env, object, key, field);
}

void SetString(napi_env env, napi_value object, const char* key, const char* value)
{
    napi_value field;
    napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &field);
    napi_set_named_property(env, object, key, field);
}

void Complete(napi_env env, napi_status status, void* data)
{
    auto* work = static_cast<Work*>(data);
    if (status != napi_ok) work->stage = "async_work";
    napi_value result;
    napi_create_object(env, &result);
    SetString(env, result, "gate", "D2.5-WINE-IPC");
    SetString(env, result, "status", std::strcmp(work->stage, "complete") == 0 ? "PASS" : "FAIL");
    SetString(env, result, "stage", work->stage);
    SetInt(env, result, "pid", work->result.pid);
    SetInt(env, result, "replyPid", work->replyPid);
    SetInt(env, result, "fdCount", work->result.fdCount);
    SetInt(env, result, "launchCode", work->launchCode);
    SetInt(env, result, "callbackCode", work->callbackCode);
    SetInt(env, result, "ipcCode", work->ipcCode);
    SetInt(env, result, "parentFdsOpen", work->parentFdsOpen ? 1 : 0);
    SetInt(env, result, "deathReceived", work->deathReceived ? 1 : 0);
    napi_resolve_deferred(env, work->deferred, result);
    napi_delete_async_work(env, work->asyncWork);
    delete work;
}

} // namespace

napi_value RunWineIpcProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) Work();
    if (!work) {
        napi_throw_error(env, nullptr, "D2.5 probe allocation failed");
        return nullptr;
    }
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        return nullptr;
    }
    napi_value name;
    napi_create_string_utf8(env, "WineHuaDirectWineIpc", NAPI_AUTO_LENGTH, &name);
    if (napi_create_async_work(env, nullptr, name, Execute, Complete,
                               work, &work->asyncWork) != napi_ok ||
        napi_queue_async_work(env, work->asyncWork) != napi_ok) {
        if (work->asyncWork) napi_delete_async_work(env, work->asyncWork);
        delete work;
        napi_throw_error(env, nullptr, "D2.5 probe queue failed");
        return nullptr;
    }
    return promise;
}

} // namespace winehua::direct
