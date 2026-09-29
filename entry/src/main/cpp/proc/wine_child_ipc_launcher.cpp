#include "wine_child_ipc_launcher.h"
#include "wine_child_ipc.h"
#include "wine_process.h"
#include "direct/direct_wine_surface_controller.h"

#include <IPCKit/ipc_kit.h>
#include <hilog/log.h>
#include <native_window/external_window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#define LOG_DOMAIN 0x2330
#define LOG_TAG "WineIpcLaunch"

namespace {

struct ChildRecord {
    int32_t pid = -1;
    OHIPCRemoteProxy* proxy = nullptr;
    OHIPCDeathRecipient* recipient = nullptr;
    std::atomic<bool> dead{false};
    std::atomic<bool> registered{false};
    std::atomic<bool> cleanupQueued{false};
    bool directVulkan = false;
    std::mutex requestMutex;
};

struct LaunchWork {
    OHIPCRemoteProxy* proxy = nullptr;
    int32_t code = -1;
    bool received = false;
};

std::mutex g_callbackMutex;
std::condition_variable g_callbackCondition;
LaunchWork* g_activeWork = nullptr;
bool g_callbackPending = false;
std::mutex g_recordsMutex;
std::unordered_map<int32_t, std::shared_ptr<ChildRecord>> g_records;

void CleanupRecord(const std::shared_ptr<ChildRecord>& record)
{
    {
        std::lock_guard<std::mutex> lock(g_recordsMutex);
        auto it = g_records.find(record->pid);
        if (it != g_records.end() && it->second == record) g_records.erase(it);
    }
    std::lock_guard<std::mutex> requestLock(record->requestMutex);
    if (record->recipient) {
        OH_IPCRemoteProxy_RemoveDeathRecipient(record->proxy, record->recipient);
        OH_IPCDeathRecipient_Destroy(record->recipient);
    }
    if (record->proxy) OH_IPCRemoteProxy_Destroy(record->proxy);
    record->proxy = nullptr;
}

bool WriteSurfaceToken(OHIPCParcel* request,
                       const winehua::wineipc::DirectSurfaceToken& token)
{
    return OH_IPCParcel_WriteInt32(request, winehua::wineipc::kVersion) == OH_IPC_SUCCESS &&
           OH_IPCParcel_WriteInt32(request, token.clientPid) == OH_IPC_SUCCESS &&
           OH_IPCParcel_WriteInt32(request, static_cast<int32_t>(token.toplevelId)) == OH_IPC_SUCCESS &&
           OH_IPCParcel_WriteInt32(request, static_cast<int32_t>(token.wlSurfaceId)) == OH_IPC_SUCCESS &&
           OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(token.generation)) == OH_IPC_SUCCESS &&
           OH_IPCParcel_WriteInt32(request, token.width) == OH_IPC_SUCCESS &&
           OH_IPCParcel_WriteInt32(request, token.height) == OH_IPC_SUCCESS;
}

bool SendSurfaceRequest(const winehua::wineipc::DirectSurfaceToken& token,
                        uint32_t code, OHNativeWindow* producerWindow)
{
    if (token.clientPid <= 0 || !token.toplevelId || !token.wlSurfaceId ||
        !token.generation || token.generation > INT64_MAX ||
        token.width <= 0 || token.height <= 0 ||
        (code == winehua::wineipc::kAttachSurface && !producerWindow))
        return false;
    std::shared_ptr<ChildRecord> record;
    {
        std::lock_guard<std::mutex> lock(g_recordsMutex);
        auto it = g_records.find(token.clientPid);
        if (it == g_records.end()) return false;
        record = it->second;
    }
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    bool ok = request && reply && WriteSurfaceToken(request, token);
    if (ok && code == winehua::wineipc::kAttachSurface)
        ok = OH_NativeWindow_WriteToParcel(producerWindow, request) == 0;
    int32_t childResult = -1;
    if (ok) {
        std::lock_guard<std::mutex> lock(record->requestMutex);
        ok = !record->dead.load(std::memory_order_acquire) && record->proxy &&
             OH_IPCRemoteProxy_SendRequest(record->proxy, code, request, reply, nullptr) == OH_IPC_SUCCESS &&
             OH_IPCParcel_ReadInt32(reply, &childResult) == OH_IPC_SUCCESS && childResult == 0;
    }
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return ok;
}

void MaybeHandleDeath(const std::shared_ptr<ChildRecord>& record)
{
    if (!record->dead.load(std::memory_order_acquire) ||
        !record->registered.load(std::memory_order_acquire) ||
        record->cleanupQueued.exchange(true, std::memory_order_acq_rel))
        return;
    // Removing a death recipient from its own callback can re-enter IPC internals.
    // Finish process bookkeeping and proxy cleanup on another thread.
    std::thread([record] {
        NoteCreateNcpDeath(record->pid);
        CleanupRecord(record);
    }).detach();
}

void OnChildDeath(void* data)
{
    auto record = *static_cast<std::shared_ptr<ChildRecord>*>(data);
    record->dead.store(true, std::memory_order_release);
    MaybeHandleDeath(record);
}

void OnRecipientDestroyed(void* data)
{
    delete static_cast<std::shared_ptr<ChildRecord>*>(data);
}

void OnChildStarted(int32_t code, OHIPCRemoteProxy* proxy)
{
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_activeWork) {
        g_activeWork->code = code;
        g_activeWork->proxy = proxy;
        g_activeWork->received = true;
        g_callbackCondition.notify_one();
        return;
    }
    g_callbackPending = false;
    if (proxy) OH_IPCRemoteProxy_Destroy(proxy);
}

} // namespace

int32_t StartWineChildViaIpc(const NativeChildProcess_Args& args, int32_t* childPid)
{
    if (!childPid || !args.entryParams) return NCP_ERR_INVALID_PARAM;
    *childPid = -1;
    if (std::strlen(args.entryParams) > 16384) return NCP_ERR_INVALID_PARAM;
    int32_t count = 0;
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (++count > winehua::wineipc::kMaxFds || !node->fdName ||
            !node->fdName[0] || std::strlen(node->fdName) > 20 || node->fd < 0)
            return NCP_ERR_INVALID_PARAM;
    }
    LaunchWork work;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        if (g_callbackPending) return NCP_ERR_BUSY;
        g_callbackPending = true;
        g_activeWork = &work;
    }
    int32_t code = OH_Ability_CreateNativeChildProcess("libwine_child.so", OnChildStarted);
    if (code != NCP_NO_ERROR) {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        g_activeWork = nullptr;
        g_callbackPending = false;
        return code;
    }
    {
        std::unique_lock<std::mutex> lock(g_callbackMutex);
        if (!g_callbackCondition.wait_for(lock, std::chrono::seconds(15),
                                          [&work] { return work.received; })) {
            g_activeWork = nullptr; // late callback destroys its proxy
            return NCP_ERR_TIMEOUT;
        }
        g_activeWork = nullptr;
        g_callbackPending = false;
    }
    if (work.code != NCP_NO_ERROR || !work.proxy) {
        if (work.proxy) OH_IPCRemoteProxy_Destroy(work.proxy);
        return work.code != NCP_NO_ERROR ? work.code : NCP_ERR_CONNECTION_FAILED;
    }

    auto record = std::make_shared<ChildRecord>();
    record->proxy = work.proxy;
    {
        const char* marker = std::strstr(args.entryParams,
            "|__env=WINEHUA_VULKAN_BACKEND=direct");
        record->directVulkan = marker &&
            (marker[sizeof("|__env=WINEHUA_VULKAN_BACKEND=direct") - 1] == '|' ||
             marker[sizeof("|__env=WINEHUA_VULKAN_BACKEND=direct") - 1] == '\0');
    }
    auto* holder = new std::shared_ptr<ChildRecord>(record);
    record->recipient = OH_IPCDeathRecipient_Create(OnChildDeath, OnRecipientDestroyed, holder);
    if (!record->recipient) {
        delete holder;
        OH_IPCRemoteProxy_Destroy(work.proxy);
        return NCP_ERR_INTERNAL;
    }
    if (OH_IPCRemoteProxy_AddDeathRecipient(work.proxy, record->recipient) != OH_IPC_SUCCESS) {
        OH_IPCDeathRecipient_Destroy(record->recipient);
        OH_IPCRemoteProxy_Destroy(work.proxy);
        return NCP_ERR_CONNECTION_FAILED;
    }

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    bool written = request && reply &&
        OH_IPCParcel_WriteInt32(request, winehua::wineipc::kVersion) == OH_IPC_SUCCESS &&
        OH_IPCParcel_WriteString(request, args.entryParams) == OH_IPC_SUCCESS;
    written = written && count <= winehua::wineipc::kMaxFds &&
        OH_IPCParcel_WriteInt32(request, count) == OH_IPC_SUCCESS;
    for (auto* node = args.fdList.head; written && node; node = node->next) {
        written = node->fdName && node->fd >= 0 &&
            OH_IPCParcel_WriteString(request, node->fdName) == OH_IPC_SUCCESS &&
            OH_IPCParcel_WriteFileDescriptor(request, node->fd) == OH_IPC_SUCCESS;
    }
    int32_t pid = -1;
    if (written &&
        OH_IPCRemoteProxy_SendRequest(work.proxy, winehua::wineipc::kBootstrap,
                                      request, reply, nullptr) == OH_IPC_SUCCESS &&
        OH_IPCParcel_ReadInt32(reply, &pid) == OH_IPC_SUCCESS && pid > 0) {
        record->pid = pid;
        {
            std::lock_guard<std::mutex> lock(g_recordsMutex);
            g_records[pid] = record;
        }
        *childPid = pid;
        code = NCP_NO_ERROR;
    } else {
        code = NCP_ERR_CONNECTION_FAILED;
    }
    if (request) OH_IPCParcel_Destroy(request);
    if (reply) OH_IPCParcel_Destroy(reply);
    if (code != NCP_NO_ERROR) CleanupRecord(record);
    return code;
}

void MarkWineIpcChildRegistered(int32_t childPid)
{
    std::shared_ptr<ChildRecord> record;
    {
        std::lock_guard<std::mutex> lock(g_recordsMutex);
        auto it = g_records.find(childPid);
        if (it != g_records.end()) record = it->second;
    }
    if (record) {
        record->registered.store(true, std::memory_order_release);
        MaybeHandleDeath(record);
        DirectWineSurfaceChildReady(static_cast<uint32_t>(childPid));
    }
}

bool WineIpcChildUsesDirectVulkan(int32_t childPid)
{
    std::lock_guard<std::mutex> lock(g_recordsMutex);
    auto it = g_records.find(childPid);
    return it != g_records.end() && it->second->directVulkan &&
           !it->second->dead.load(std::memory_order_acquire);
}

bool AttachWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token,
                             OHNativeWindow* producerWindow)
{
    return SendSurfaceRequest(token, winehua::wineipc::kAttachSurface, producerWindow);
}

bool DetachWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token)
{
    return SendSurfaceRequest(token, winehua::wineipc::kDetachSurface, nullptr);
}

bool ResizeWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token)
{
    return SendSurfaceRequest(token, winehua::wineipc::kResizeSurface, nullptr);
}

bool QueryWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token)
{
    return SendSurfaceRequest(token, winehua::wineipc::kQuerySurface, nullptr);
}

bool FinishWineDirectSurfaceProbe(int32_t childPid)
{
    if (childPid <= 0) return false;
    std::shared_ptr<ChildRecord> record;
    {
        std::lock_guard<std::mutex> lock(g_recordsMutex);
        auto it = g_records.find(childPid);
        if (it == g_records.end()) return false;
        record = it->second;
    }
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t result = -1;
    bool ok = request && reply &&
        OH_IPCParcel_WriteInt32(request, winehua::wineipc::kVersion) == OH_IPC_SUCCESS;
    if (ok) {
        std::lock_guard<std::mutex> lock(record->requestMutex);
        ok = !record->dead.load(std::memory_order_acquire) && record->proxy &&
             OH_IPCRemoteProxy_SendRequest(record->proxy,
                 winehua::wineipc::kFinishSurfaceProbe, request, reply, nullptr) == OH_IPC_SUCCESS &&
             OH_IPCParcel_ReadInt32(reply, &result) == OH_IPC_SUCCESS && result == 0;
    }
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return ok;
}
