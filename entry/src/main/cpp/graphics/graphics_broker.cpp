#include "graphics_broker.h"

#include "common/fs_utils.h"
#include "common/process_utils.h"
#include "common/string_utils.h"
#include "common/wait_utils.h"
#include "compositor/wayland_server.h"

#include <AbilityKit/native_child_process.h>
#include "phone_adapter/phone_adapter.h"
#include <IPCKit/ipc_kit.h>
#include <native_window/external_window.h>
#include "virgl_ipc_protocol.h"
#include "wine/wine_env.h"

// ---- 与 OH_IPCRemoteProxy_* 签名兼容的包装：真 proxy 走原 API，dummy 走 socket relay ----
static int SendVirglRequestLocked(OHIPCRemoteProxy* proxy, uint32_t code,
                                  const OHIPCParcel* data, OHIPCParcel* reply,
                                  OH_IPC_MessageOption* option) {
    if (!PhoneAdapter_IsDummyProxy(proxy)) {
        return OH_IPCRemoteProxy_SendRequest(proxy, code, data, reply, option);
    }
    if (code == winehua::virgl_ipc::kAttachSurfaceRequest) {
        // OHNativeWindow 依赖 Binder 跨进程，手机 fork 路径下不可达 → 伪造失败，走 shm
        OH_LOG_WARN(LOG_APP, "[PhoneVirgl] AttachSurface denied in phone mode, fallback to shm");
        if (reply) OH_IPCParcel_WriteInt32(reply, kPhoneVirglAttachDenied);
        return OH_IPC_SUCCESS;
    }
    return PhoneVirgl_RelayRequest(code, data, reply);  // → phone_adapter/phone_virgl_relay.cpp
}

static void VirglProxyDestroy(OHIPCRemoteProxy* proxy) {
    if (PhoneAdapter_IsDummyProxy(proxy)) return;   // dummy proxy 的 socket 由手机适配层管理
    OH_IPCRemoteProxy_Destroy(proxy);
}

static int VirglProxyIsRemoteDead(OHIPCRemoteProxy* proxy) {
    if (!PhoneAdapter_IsDummyProxy(proxy)) return OH_IPCRemoteProxy_IsRemoteDead(proxy);
    int fd = PhoneAdapter_GetConfigSocket();
    if (fd < 0) return 1;
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return 1;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    return 0;
}

#include "virgl_ipc_protocol.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <fcntl.h>
#include <signal.h>
#include <thread>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_GFX"
#include <hilog/log.h>

namespace winehua {

namespace {


constexpr const char* VIRGL_SERVER_PROGRAM = "virgl_test_server";
constexpr const char* VIRGL_VTEST_LIBRARY = "libwinehua_vtest_server.so";
constexpr const char* GUEST_GFX_DIRNAME = "guest_gfx";
constexpr const char* GUEST_GFX_ENVFILE = "winehua-guest-gfx.env";
constexpr const char* ZERO_COPY_READY_DIR = "/data/storage/el2/base/cache";
constexpr const char* ZERO_COPY_READY_PREFIX = "winehua_zc_surface_";

using VirglInProcessRunFn = int (*)(
    const char*, const char*, const char*, const char*, const char*,
    const char*, const char*, const char*, const char*, const char*,
    const char*);
using VirglInProcessStopFn = int (*)();
using VirglInProcessAttachFn = int (*)(uint64_t, uint64_t, uint32_t, OHNativeWindow*);
using VirglInProcessDetachFn = int (*)(uint64_t);
using VirglInProcessSetFramePeriodFn = int (*)(uint64_t, uint64_t);
using VirglInProcessQueryFn = int (*)(virgl_ipc::SurfaceQueryReply*);
using VirglInProcessResetFn = void (*)();

std::string ZeroCopyReadyPath(uint64_t surfaceKey)
{
    return std::string(ZERO_COPY_READY_DIR) + "/" + ZERO_COPY_READY_PREFIX +
           std::to_string(surfaceKey) + ".ready";
}

void RemoveStaleZeroCopyMarkers()
{
    DIR* dir = opendir(ZERO_COPY_READY_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.rfind(ZERO_COPY_READY_PREFIX, 0) != 0 ||
            name.size() < 6 || name.compare(name.size() - 6, 6, ".ready") != 0)
            continue;
        unlink((std::string(ZERO_COPY_READY_DIR) + "/" + name).c_str());
    }
    closedir(dir);
}


bool LoadGuestReceiverEnvFile(const std::string& receiverDir,
                              const std::string& envPath,
                              std::vector<std::string>& envLines,
                              std::string& mode)
{
    std::ifstream input(envPath);
    std::string line;

    if (!input.is_open()) return false;

    while (std::getline(input, line))
    {
        size_t equals;
        std::string key;
        std::string value;

        line = TrimCopy(line);
        if (line.empty() || line[0] == '#') continue;
        if (!line.compare(0, 7, "export ")) line = TrimCopy(line.substr(7));

        equals = line.find('=');
        if (equals == std::string::npos) continue;

        key = TrimCopy(line.substr(0, equals));
        value = TrimCopy(line.substr(equals + 1));
        if (key.empty()) continue;

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        ReplaceAll(value, "$ORIGIN", receiverDir);
        if (key == "WINEHUA_GUEST_GFX_MODE") mode = value;

        if (key == "LD_LIBRARY_PATH" || key == "BOX64_LD_LIBRARY_PATH") continue;
        envLines.push_back(key + "=" + value);
    }

    return true;
}


} // namespace

GraphicsBroker& GraphicsBroker::GetInstance()
{
    static GraphicsBroker broker;
    return broker;
}

GraphicsBroker::GraphicsBroker()
{
    const char* requested = std::getenv("WINEHUA_GRAPHICS_BACKEND");
    GraphicsBackend backend;

    if (requested && ParseBackendName(requested, &backend)) {
        requestedBackend_ = backend;
    }
}

bool GraphicsBroker::StartVirglInProcessHostLocked(const VirglHostConfig& config)
{
    std::string configError;
    if (!ValidateVirglHostConfig(config, &configError))
    {
        lastError_ = "invalid in-process VirGL host config: " + configError;
        OH_LOG_ERROR(LOG_APP, "[GraphicsBroker] %{public}s", lastError_.c_str());
        return false;
    }
    if (!virglInProcessHandle_)
    {
        const std::string bundleDir = CurrentSharedObjectDir();
        const std::string absolutePath = bundleDir.empty()
            ? std::string() : bundleDir + "/libvirgl_child.so";
        if (!absolutePath.empty())
            virglInProcessHandle_ = dlopen(absolutePath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!virglInProcessHandle_)
            virglInProcessHandle_ = dlopen("libvirgl_child.so", RTLD_NOW | RTLD_LOCAL);
        if (!virglInProcessHandle_)
        {
            const char* error = dlerror();
            lastError_ = std::string("failed to load in-process virgl host: ") +
                (error ? error : "unknown dynamic linker error");
            OH_LOG_ERROR(LOG_APP, "[GraphicsBroker] %{public}s", lastError_.c_str());
            return false;
        }
    }

    auto runFn = reinterpret_cast<VirglInProcessRunFn>(
        dlsym(virglInProcessHandle_, "WinehuaVirgl_RunConfiguredHost"));
    virglInProcessStop_ = dlsym(virglInProcessHandle_, "WinehuaVirgl_RequestStop");
    virglInProcessAttach_ = dlsym(virglInProcessHandle_, "WinehuaVirgl_AttachSurfaceTarget");
    virglInProcessDetach_ = dlsym(virglInProcessHandle_, "WinehuaVirgl_DetachSurfaceTarget");
    virglInProcessSetFramePeriod_ = dlsym(
        virglInProcessHandle_, "WinehuaVirgl_SetSurfaceFramePeriod");
    virglInProcessQuery_ = dlsym(virglInProcessHandle_, "WinehuaVirgl_QuerySurfaces");
    virglInProcessReset_ = dlsym(virglInProcessHandle_, "WinehuaVirgl_ResetSurfaces");
    if (!runFn || !virglInProcessStop_ ||
        !virglInProcessAttach_ || !virglInProcessDetach_ ||
        !virglInProcessSetFramePeriod_ || !virglInProcessQuery_ || !virglInProcessReset_)
    {
        const char* error = dlerror();
        lastError_ = std::string("in-process virgl host exports are incomplete: ") +
            (error ? error : "missing symbol");
        OH_LOG_ERROR(LOG_APP, "[GraphicsBroker] %{public}s", lastError_.c_str());
        return false;
    }

    virglServerPid_ = getpid();
    virglServerUsesNcp_ = false;
    virglServerUsesIpc_ = false;
    virglServerUsesInProcess_.store(true, std::memory_order_release);
    virglServerRunning_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
        virglIpcConfigured_ = true;
        virglIpcCallbackComplete_ = true;
    }

    std::thread([this, runFn, config]() {
        const uint64_t configHash = FingerprintVirglHostConfig(config);
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] phone in-process VirGL host starting config=0x%{public}llx",
                    static_cast<unsigned long long>(configHash));
        const int result = runFn(
            config.helperPath.c_str(), config.socketPath.c_str(),
            config.libraryPath.c_str(), config.syncMode.c_str(),
            config.logPath.c_str(), config.shadowMode.c_str(),
            config.shadowTrace.c_str(), config.presentMode.c_str(),
            config.shadowMergeRanges.c_str(),
            config.descriptorUpdateSerialize.c_str(),
            config.gpuUploadWait.c_str());
        {
            std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
            virglIpcConfigured_ = false;
        }
        OH_LOG_WARN(LOG_APP,
                    "[GraphicsBroker] phone in-process VirGL host exited result=%{public}d config=0x%{public}llx",
                    result, static_cast<unsigned long long>(configHash));
        virglServerRunning_.store(false, std::memory_order_release);
        virglIpcCondition_.notify_all();
    }).detach();
    return true;
}

void GraphicsBroker::ResetVirglInProcessSurfacesLocked()
{
    auto resetFn = reinterpret_cast<VirglInProcessResetFn>(virglInProcessReset_);
    if (resetFn) resetFn();
    for (uint64_t surfaceKey : zeroCopyAttachedSurfaces_)
        unlink(ZeroCopyReadyPath(surfaceKey).c_str());
    zeroCopyAttachedSurfaces_.clear();
}

void GraphicsBroker::OnVirglIpcProcessStarted(int errorCode, OHIPCRemoteProxy* remoteProxy)
{
    GraphicsBroker& broker = GetInstance();
    std::unique_lock<std::mutex> lock(broker.virglIpcMutex_);

    broker.virglIpcError_ = errorCode;
    if (!broker.virglIpcAcceptCallback_ || errorCode != NCP_NO_ERROR || !remoteProxy)
    {
        if (remoteProxy) VirglProxyDestroy(remoteProxy);
        broker.virglIpcConfigured_ = false;
        broker.virglIpcCallbackComplete_ = true;
        lock.unlock();
        broker.virglIpcCondition_.notify_all();
        return;
    }

    if (broker.virglRemoteProxy_) VirglProxyDestroy(broker.virglRemoteProxy_);
    broker.virglRemoteProxy_ = remoteProxy;
    broker.virglIpcConfigured_ = broker.SendVirglConfigureLocked();
    broker.virglIpcCallbackComplete_ = true;

    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] child IPC callback err=%{public}d configured=%{public}s",
                errorCode, broker.virglIpcConfigured_ ? "PASS" : "FAIL");
    lock.unlock();
    broker.virglIpcCondition_.notify_all();
}

bool GraphicsBroker::SendVirglConfigureLocked()
{
    if (!virglRemoteProxy_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.helperPath.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.socketPath.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.libraryPath.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.syncMode.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.logPath.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.shadowMode.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.shadowTrace.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(request, virglHostConfig_.presentMode.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(
            request, virglHostConfig_.shadowMergeRanges.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(
            request, virglHostConfig_.descriptorUpdateSerialize.c_str());
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteString(
            request, virglHostConfig_.gpuUploadWait.c_str());

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = SendVirglRequestLocked(
            virglRemoteProxy_, virgl_ipc::kConfigureRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::SendVirglTargetLocked(uint64_t surfaceKey,
                                           OHNativeWindow* producerWindow,
                                           uint64_t framePeriodNs,
                                           uint32_t flags)
{
    if (!virglIpcConfigured_ || !producerWindow || !surfaceKey)
        return false;
    if (virglServerUsesInProcess_.load(std::memory_order_acquire))
    {
        /* The phone runtime keeps the presenter in this process so its
         * NativeWindow remains valid. SurfaceQueuePresenterManager selects
         * VenusSurfaceQueueTarget for kSurfaceVulkan, just as it selects the
         * GLES target for VirGL surfaces. Rejecting the flag here leaves DXVK
         * presenting only the empty Wayland buffer.
         *
         * OH_NativeImage owns the reference returned by AcquireNativeWindow,
         * while both presenter targets destroy the reference they retain at
         * detach. Give the in-process target a distinct reference; otherwise
         * closing a presented Wine window destroys the NativeImage-owned
         * window and the subsequent NativeImage teardown can abort the App. */
        const int32_t referenceResult =
            OH_NativeWindow_NativeObjectReference(producerWindow);
        if (referenceResult != 0)
        {
            OH_LOG_WARN(LOG_APP,
                        "[VIRGL-ZC][MAIN] direct attach reference failed key=%{public}llu result=%{public}d",
                        static_cast<unsigned long long>(surfaceKey), referenceResult);
            return false;
        }
        auto attachFn = reinterpret_cast<VirglInProcessAttachFn>(virglInProcessAttach_);
        const int result = attachFn ? attachFn(surfaceKey, framePeriodNs, flags, producerWindow) : -1;
        if (result != 0)
            OH_NativeWindow_NativeObjectUnreference(producerWindow);
        OH_LOG_INFO(LOG_APP,
                    "[VIRGL-ZC][MAIN] direct attach surface_key=%{public}llu "
                    "period_ns=%{public}llu result=%{public}d",
                    static_cast<unsigned long long>(surfaceKey),
                    static_cast<unsigned long long>(framePeriodNs), result);
        return result == 0;
    }
    if (!virglRemoteProxy_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(framePeriodNs));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt32(request, static_cast<int32_t>(flags));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_NativeWindow_WriteToParcel(producerWindow, request);

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = SendVirglRequestLocked(
            virglRemoteProxy_, virgl_ipc::kAttachSurfaceRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] attach surface_key=%{public}llu period_ns=%{public}llu "
                "write=%{public}d send=%{public}d child=%{public}d",
                static_cast<unsigned long long>(surfaceKey),
                static_cast<unsigned long long>(framePeriodNs),
                writeResult, sendResult, childResult);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::SendVirglFramePeriodLocked(uint64_t surfaceKey,
                                                uint64_t framePeriodNs)
{
    if (!virglIpcConfigured_ || !surfaceKey || !framePeriodNs)
        return false;
    if (virglServerUsesInProcess_.load(std::memory_order_acquire))
    {
        auto setFn = reinterpret_cast<VirglInProcessSetFramePeriodFn>(
            virglInProcessSetFramePeriod_);
        return setFn && setFn(surfaceKey, framePeriodNs) == 0;
    }
    if (!virglRemoteProxy_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(framePeriodNs));

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = SendVirglRequestLocked(
            virglRemoteProxy_, virgl_ipc::kSetFramePeriodRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::SendVirglDetachLocked(uint64_t surfaceKey)
{
    if (!virglIpcConfigured_) return false;
    if (virglServerUsesInProcess_.load(std::memory_order_acquire))
    {
        auto detachFn = reinterpret_cast<VirglInProcessDetachFn>(virglInProcessDetach_);
        return detachFn && detachFn(surfaceKey) == 0;
    }
    if (!virglRemoteProxy_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t writeResult = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (writeResult == OH_IPC_SUCCESS)
        writeResult = OH_IPCParcel_WriteInt64(request, static_cast<int64_t>(surfaceKey));

    int32_t childResult = -1;
    int32_t sendResult = writeResult;
    if (writeResult == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        sendResult = SendVirglRequestLocked(
            virglRemoteProxy_, virgl_ipc::kDetachSurfaceRequest,
            request, reply, &option);
        if (sendResult == OH_IPC_SUCCESS)
            sendResult = OH_IPCParcel_ReadInt32(reply, &childResult);
    }

    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return sendResult == OH_IPC_SUCCESS && childResult == 0;
}

bool GraphicsBroker::AttachZeroCopyTarget(uint64_t surfaceKey,
                                          OHNativeWindow* producerWindow,
                                          uint64_t framePeriodNs)
{
    if (!surfaceKey || !producerWindow || GetState().active != GraphicsBackend::Virgl)
        return false;

    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (zeroCopyAttachedSurfaces_.count(surfaceKey)) return true;
    uint32_t flags = vulkanPresentMode_.load(std::memory_order_acquire)
        ? virgl_ipc::kSurfaceVulkan : 0;
    if (virglServerUsesInProcess_.load(std::memory_order_acquire))
        flags |= virgl_ipc::kSurfaceNativeObjectReference;
    if (!SendVirglTargetLocked(surfaceKey, producerWindow, framePeriodNs, flags)) return false;
    zeroCopyAttachedSurfaces_.insert(surfaceKey);
    return true;
}

void GraphicsBroker::SetZeroCopyFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs)
{
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (!surfaceKey || !framePeriodNs || !zeroCopyAttachedSurfaces_.count(surfaceKey)) return;
    if (!SendVirglFramePeriodLocked(surfaceKey, framePeriodNs))
    {
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] frame period update failed key=%{public}llu period_ns=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey),
                    static_cast<unsigned long long>(framePeriodNs));
    }
}

void GraphicsBroker::DetachZeroCopyTarget(uint64_t surfaceKey)
{
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (!surfaceKey || !zeroCopyAttachedSurfaces_.count(surfaceKey)) return;

    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] broker detach begin key=%{public}llu in_process=%{public}d",
                static_cast<unsigned long long>(surfaceKey),
                virglServerUsesInProcess_.load(std::memory_order_acquire));
    const bool detached = SendVirglDetachLocked(surfaceKey);
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] broker detach end key=%{public}llu result=%{public}d",
                static_cast<unsigned long long>(surfaceKey), detached);
    zeroCopyAttachedSurfaces_.erase(surfaceKey);
    unlink(ZeroCopyReadyPath(surfaceKey).c_str());
    OH_LOG_INFO(LOG_APP, "[VIRGL-ZC][MAIN] detached surface_key=%{public}llu",
                static_cast<unsigned long long>(surfaceKey));
}

bool GraphicsBroker::QueryZeroCopySurfaces(std::vector<ZeroCopySurfaceInfo>& surfaces) const
{
    surfaces.clear();
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    if (!virglIpcConfigured_) return false;
    if (virglServerUsesInProcess_.load(std::memory_order_acquire))
    {
        auto queryFn = reinterpret_cast<VirglInProcessQueryFn>(virglInProcessQuery_);
        virgl_ipc::SurfaceQueryReply queryReply;
        if (!queryFn || queryFn(&queryReply) != 0 ||
            queryReply.magic != virgl_ipc::kMagic ||
            queryReply.version != static_cast<uint32_t>(virgl_ipc::kProtocolVersion) ||
            queryReply.size != sizeof(queryReply) ||
            queryReply.count > virgl_ipc::kMaxSurfaces)
            return false;
        surfaces.reserve(queryReply.count);
        for (uint32_t i = 0; i < queryReply.count; ++i)
        {
            const auto& item = queryReply.surfaces[i];
            surfaces.push_back({item.surfaceKey, item.clientPid, item.surfaceId,
                                item.width, item.height, item.serial,
                                (item.flags & virgl_ipc::kSurfaceAttached) != 0,
                                (item.flags & virgl_ipc::kSurfaceVulkan) != 0});
        }
        return true;
    }
    if (!virglRemoteProxy_) return false;

    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    int32_t result = request
        ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
        : OH_IPC_MEM_ALLOCATOR_ERROR;
    if (result == OH_IPC_SUCCESS && reply)
    {
        OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
        result = SendVirglRequestLocked(
            virglRemoteProxy_, virgl_ipc::kQuerySurfacesRequest,
            request, reply, &option);
    }

    virgl_ipc::SurfaceQueryReply queryReply;
    if (result == OH_IPC_SUCCESS)
    {
        const uint8_t* bytes = OH_IPCParcel_ReadBuffer(
            reply, static_cast<int32_t>(sizeof(queryReply)));
        if (!bytes)
        {
            result = OH_IPC_PARCEL_READ_ERROR;
        }
        else
        {
            memcpy(&queryReply, bytes, sizeof(queryReply));
            if (queryReply.magic != virgl_ipc::kMagic ||
                queryReply.version != static_cast<uint32_t>(virgl_ipc::kProtocolVersion) ||
                queryReply.size != sizeof(queryReply) ||
                queryReply.count > virgl_ipc::kMaxSurfaces)
                result = OH_IPC_PARCEL_READ_ERROR;
        }
    }

    if (result == OH_IPC_SUCCESS)
    {
        surfaces.reserve(queryReply.count);
        for (uint32_t i = 0; i < queryReply.count; ++i)
        {
            const auto& item = queryReply.surfaces[i];
            surfaces.push_back({item.surfaceKey, item.clientPid, item.surfaceId,
                                item.width, item.height, item.serial,
                                (item.flags & virgl_ipc::kSurfaceAttached) != 0,
                                (item.flags & virgl_ipc::kSurfaceVulkan) != 0});
        }
    }
    if (reply) OH_IPCParcel_Destroy(reply);
    if (request) OH_IPCParcel_Destroy(request);
    return result == OH_IPC_SUCCESS;
}

void GraphicsBroker::SetZeroCopySurfaceReady(uint64_t surfaceKey, bool ready)
{
    if (!surfaceKey) return;
    const std::string path = ZeroCopyReadyPath(surfaceKey);
    if (!ready)
    {
        unlink(path.c_str());
        return;
    }

    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
    {
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] ready marker create failed key=%{public}llu errno=%{public}d",
                    static_cast<unsigned long long>(surfaceKey), errno);
        return;
    }
    static constexpr char payload[] = "ready\n";
    const ssize_t written = write(fd, payload, sizeof(payload) - 1);
    const int writeError = written == static_cast<ssize_t>(sizeof(payload) - 1)
        ? 0 : (errno ? errno : EIO);
    close(fd);
    if (writeError)
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] ready marker write failed key=%{public}llu errno=%{public}d",
                    static_cast<unsigned long long>(surfaceKey), writeError);
}

void GraphicsBroker::ShutdownVirglIpc()
{
    std::lock_guard<std::mutex> lock(virglIpcMutex_);
    virglIpcAcceptCallback_ = false;
    if (virglRemoteProxy_)
    {
        OHIPCParcel* request = OH_IPCParcel_Create();
        OHIPCParcel* reply = OH_IPCParcel_Create();
        if (request && reply &&
            OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion) == OH_IPC_SUCCESS)
        {
            OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
            SendVirglRequestLocked(
                virglRemoteProxy_, virgl_ipc::kShutdownRequest,
                request, reply, &option);
        }
        if (reply) OH_IPCParcel_Destroy(reply);
        if (request) OH_IPCParcel_Destroy(request);
        VirglProxyDestroy(virglRemoteProxy_);
        virglRemoteProxy_ = nullptr;
    }
    virglIpcConfigured_ = false;
    virglIpcCallbackComplete_ = false;
    for (uint64_t surfaceKey : zeroCopyAttachedSurfaces_)
        unlink(ZeroCopyReadyPath(surfaceKey).c_str());
    zeroCopyAttachedSurfaces_.clear();
}

void GraphicsBroker::SetWineRuntimeBinaryDir(const std::string& wineBinDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    wineRuntimeBinDir_ = wineBinDir;
    virglServerProgramPath_.clear();
    virglVtestLibraryPath_.clear();
    if (!wineRuntimeBinDir_.empty())
    {
        std::string bundleDir = CurrentSharedObjectDir();
        std::string bundleVtestLibrary;

        virglServerProgramPath_ = wineRuntimeBinDir_ + "/" + VIRGL_SERVER_PROGRAM;
        if (!bundleDir.empty())
            bundleVtestLibrary = bundleDir + "/" + VIRGL_VTEST_LIBRARY;

        if (FileExists(bundleVtestLibrary))
            virglVtestLibraryPath_ = bundleVtestLibrary;

        OH_LOG_INFO(LOG_APP, "[GraphicsBroker] host helper=%{public}s server=%{public}s",
                    virglVtestLibraryPath_.empty() ? "(none)" : virglVtestLibraryPath_.c_str(),
                    virglServerProgramPath_.c_str());
    }
    RefreshGuestReceiverStateLocked();
}

bool GraphicsBroker::EnsureStarted(const std::string& runtimeDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!started_) RemoveStaleZeroCopyMarkers();

    if (!EnsureRuntimeLocked(runtimeDir)) {
        lastError_ = "failed to prepare graphics runtime directory";
        return false;
    }

    RefreshGuestReceiverStateLocked();
    started_ = true;
    if (requestedBackend_ == GraphicsBackend::Virgl) {
        RefreshVirglStateLocked();
        StartVirglSocketServerLocked();
    } else {
        lastError_.clear();
    }
    UpdateActiveBackendLocked();
    return true;
}

void GraphicsBroker::Stop()
{
    std::string socketPath;
    int serverPid = -1;
    bool serverUsesNcp = false;
    bool serverUsesIpc = false;
    bool serverUsesInProcess = false;
    VirglInProcessStopFn inProcessStopFn = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        serverUsesInProcess = virglServerUsesInProcess_.load(std::memory_order_acquire);
        if (!serverUsesInProcess)
            virglServerRunning_.store(false, std::memory_order_release);
        socketPath = virglSocketPath_;
        serverPid = virglServerPid_;
        serverUsesNcp = virglServerUsesNcp_;
        serverUsesIpc = virglServerUsesIpc_;
        if (serverUsesInProcess)
        {
            inProcessStopFn = reinterpret_cast<VirglInProcessStopFn>(
                virglInProcessStop_);
            virglSocketReady_ = false;
        }
        else
        {
            virglServerPid_ = -1;
            virglServerUsesNcp_ = false;
            virglServerUsesIpc_ = false;
            virglSocketReady_ = false;
        }
        activeBackend_ = GraphicsBackend::Shm;
        started_ = false;
        runtimeReady_ = false;
    }

    if (serverUsesInProcess)
    {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        int requestResult = 0;

        /* The broker marks the host running immediately before launching its
         * thread. Retry until virgl_child has entered its matching run so an
         * immediate Stop cannot miss that narrow scheduling window. */
        while (virglServerRunning_.load(std::memory_order_acquire) &&
               requestResult == 0 &&
               std::chrono::steady_clock::now() < deadline)
        {
            if (inProcessStopFn)
                requestResult = inProcessStopFn();
            if (requestResult == 0)
            {
                std::unique_lock<std::mutex> ipcLock(virglIpcMutex_);
                virglIpcCondition_.wait_for(
                    ipcLock, std::chrono::milliseconds(10), [this]() {
                        return !virglServerRunning_.load(std::memory_order_acquire);
                    });
            }
        }

        std::unique_lock<std::mutex> ipcLock(virglIpcMutex_);
        const bool exited = virglIpcCondition_.wait_until(
            ipcLock, deadline, [this]() {
                return !virglServerRunning_.load(std::memory_order_acquire);
            });
        if (exited)
            ResetVirglInProcessSurfacesLocked();
        ipcLock.unlock();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exited)
            {
                virglServerPid_ = -1;
                virglServerUsesInProcess_.store(false, std::memory_order_release);
                virglServerRunning_.store(false, std::memory_order_release);
                virglServerUsesNcp_ = false;
                virglServerUsesIpc_ = false;
                lastError_.clear();
            }
            else
            {
                lastError_ = "timed out waiting for in-process VirGL host shutdown";
            }
        }

        if (exited && !socketPath.empty()) unlink(socketPath.c_str());
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] in-process VirGL stop request=%{public}d exited=%{public}d",
                    requestResult, exited ? 1 : 0);
        return;
    }
    if (serverUsesIpc)
    {
        ShutdownVirglIpc();
    }
    else if (serverPid > 0)
    {
        TerminateTrackedProcess(serverPid, serverUsesNcp);
    }
    if (!socketPath.empty()) unlink(socketPath.c_str());
}

void GraphicsBroker::SetRequestedBackend(GraphicsBackend backend)
{
    std::lock_guard<std::mutex> lock(mutex_);

    requestedBackend_ = backend;
    loggedVirglFallback_ = false;
    if (started_ && requestedBackend_ == GraphicsBackend::Virgl) {
        RefreshVirglStateLocked();
        StartVirglSocketServerLocked();
    } else if (requestedBackend_ == GraphicsBackend::Shm) {
        lastError_.clear();
    }
    UpdateActiveBackendLocked();
}

void GraphicsBroker::SetVulkanPresentMode(bool enabled)
{
    vulkanPresentMode_.store(enabled, std::memory_order_release);
}

bool GraphicsBroker::IsVulkanPresentMode() const
{
    return vulkanPresentMode_.load(std::memory_order_acquire);
}

GraphicsBackendState GraphicsBroker::GetState() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    GraphicsBackendState state;
    state.requested = requestedBackend_;
    state.active = activeBackend_;
    state.runtimeReady = runtimeReady_;
    state.guestReceiverPresent = guestReceiverPresent_;
    state.guestReceiverRuntimeDir = guestReceiverRuntimeDir_;
    state.guestReceiverMode = guestReceiverMode_;
    state.guestReceiverError = guestReceiverError_;
    state.virglSocketReady = virglSocketReady_;
    state.virglLibraryPresent = virglLibraryPresent_;
    {
        std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
        state.zeroCopyFramePath = state.active == GraphicsBackend::Virgl && virglIpcConfigured_;
    }
    state.runtimeDir = runtimeDir_;
    state.virglSocketPath = virglSocketPath_;
    state.virglLibraryPath = virglLibraryPath_;
    state.frameTransportMode = state.zeroCopyFramePath
        ? "virgl_texture+surface_queue+external_oes"
        : "wl_shm+cpu_copy+gl_upload";
    state.lastError = lastError_;
    return state;
}

void GraphicsBroker::AppendWineEnv(std::vector<std::string>& env) const
{
    GraphicsBackendState state = GetState();
    std::vector<std::string> guestEnv;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        guestEnv = guestReceiverEnv_;
    }

    env.push_back("WINEHUA_GRAPHICS_BACKEND=" + std::string(BackendName(state.requested)));
    env.push_back("WINEHUA_GRAPHICS_ACTIVE=" + std::string(BackendName(state.active)));
    env.push_back(std::string("WINEHUA_SHM_FALLBACK=") + (state.active == GraphicsBackend::Shm ? "1" : "0"));
    env.push_back(std::string("WINEHUA_FRAME_ZERO_COPY=") + (state.zeroCopyFramePath ? "1" : "0"));
    env.push_back("WINEHUA_FRAME_TRANSPORT=" + state.frameTransportMode);
    env.push_back(std::string("WINEHUA_GUEST_GFX_READY=") + (state.guestReceiverPresent ? "1" : "0"));
    env.push_back("WINEHUA_GUEST_GFX_MODE=" + (state.guestReceiverMode.empty() ? std::string("stock-egl")
                                                                                : state.guestReceiverMode));
    if (!state.guestReceiverRuntimeDir.empty()) env.push_back("WINEHUA_GUEST_GFX_DIR=" + state.guestReceiverRuntimeDir);
    env.push_back(std::string("WINEHUA_VIRGL_SOCKET_READY=") + (state.virglSocketReady ? "1" : "0"));
    env.push_back(std::string("WINEHUA_VIRGL_LIBRARY_READY=") + (state.virglLibraryPresent ? "1" : "0"));
    if (!state.virglSocketPath.empty()) env.push_back("WINEHUA_VIRGL_SOCKET=" + state.virglSocketPath);
    if (!state.virglLibraryPath.empty()) env.push_back("WINEHUA_VIRGLRENDERER_LIB=" + state.virglLibraryPath);
    if (!state.lastError.empty()) env.push_back("WINEHUA_GRAPHICS_NOTE=" + state.lastError);
    env.push_back(std::string("WINEHUA_VIRGL_READY=") +
                  ((state.active == GraphicsBackend::Virgl) ? "1" : "0"));
    if (vulkanPresentMode_.load(std::memory_order_acquire))
        env.push_back("WINEHUA_VULKAN_PRESENT=1");
    if (state.active == GraphicsBackend::Virgl)
    {
        if (!state.guestReceiverRuntimeDir.empty())
        {
            std::string guestLibDir = state.guestReceiverRuntimeDir + "/lib";
            env.push_back("EGL_PLATFORM=wayland");
            if (FileExists(guestLibDir + "/libEGL.so"))
#if defined(__aarch64__) && !defined(WINEHUA_WINE_ARCH_IS_X86_64)
                // 方案③ arm64 原生: el1 dlopen (el2 被拒). 方案② (box64+wine) 走
                // #else guestLibDir — guest 是 x86_64, EGL 在 el2 由 box64 转译加载,
                // el1 的 arm64 libEGL.so 不存在, 注入会致 explorer 的 EGL 初始化失败.
                env.push_back("WINEHUA_EGL_LIBRARY_PATH=/data/storage/el1/bundle/libs/arm64/libEGL.so");
#elif defined(__x86_64__)
                env.push_back("WINEHUA_EGL_LIBRARY_PATH=/data/storage/el1/bundle/libs/x86_64/libEGL.so");
#else
                env.push_back("WINEHUA_EGL_LIBRARY_PATH=" + guestLibDir + "/libEGL.so");
#endif
#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
            // 方案② box64+wine: 非 DXVK 基线值 (8 个 lib, 不含 libvulkan)。
            // DXVK 路径下 AppendD3dBackendEnv 会用 14 个 lib (含 libvulkan) 覆盖此值。
            env.push_back("BOX64_EMULATED_LIBS=libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
                          "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
                          "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
                          "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
                          "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8");
#endif
#if defined(__aarch64__) && !defined(WINEHUA_WINE_ARCH_IS_X86_64)
            // LIBGL_DRIVERS_PATH 由 virgl 块末尾统一 Upsert 到 el1 (guestEnv push 之后), 不在此设置
#elif defined(__x86_64__)
            // LIBGL_DRIVERS_PATH 由下方 softpipe 块统一指向 el1, 不在此重复设置
#else
            // 方案② (box64+wine): dri 在 el2 guestLibDir, 由 box64 转译加载
            if (DirExists(guestLibDir + "/dri")) env.push_back("LIBGL_DRIVERS_PATH=" + guestLibDir + "/dri");
#endif
            if (DirExists(guestLibDir + "/egl")) env.push_back("EGL_DRIVERS_PATH=" + guestLibDir + "/egl");
        }
        env.push_back("WINEHUA_WAYLAND_READBACK=1");
        env.push_back("WINEHUA_GL_STALL_DIAG=1");
        env.push_back("WINEHUA_DISPLAY_FPS_FILE=C:\\windows\\temp\\winehua_display_fps.txt");
        env.push_back("WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log");
        env.push_back("WINEHUA_VTEST_PRESENT=surface-queue");
        env.push_back(std::string("WINEHUA_ZERO_COPY_READY_DIR=") + ZERO_COPY_READY_DIR);
        for (const std::string& extra : guestEnv) env.push_back(extra);
#if defined(__aarch64__) && !defined(WINEHUA_WINE_ARCH_IS_X86_64)
        // 方案③: dri 平铺在 el1 bundle。必须在 guestEnv 之后 Upsert, 盖掉
        // env 文件里已展开的 LIBGL_DRIVERS_PATH=$ORIGIN/lib/dri (el2)。
        // virpipe 的正确加载方式是 guest env 既有的
        // LIBGL_ALWAYS_SOFTWARE=1 + MESA_LOADER_DRIVER_OVERRIDE=swrast +
        // GALLIUM_DRIVER=virpipe (virpipe 是 gallium pipe, 不是独立 dri)。
        UpsertEnvLine(env, "LIBGL_DRIVERS_PATH=/data/storage/el1/bundle/libs/arm64");
        if (!state.virglSocketPath.empty()) env.push_back("VTEST_SOCKET_NAME=" + state.virglSocketPath);
#elif defined(__x86_64__)
        // HarmonyOS PC emulator express GPU cannot host GL: eglCreateContext
        // with a NULL share context and glTexImage2D with NULL pixels crash
        // Emulator.exe. Route the x86_64 guest to software rendering
        // (softpipe) instead of virpipe, and do not advertise the vtest
        // socket. arm64 real-device virgl path is unchanged.
        //
        // NOTE: 必须用 UpsertEnvLine 覆盖而不是 push_back — guest_gfx env
        // 文件 (共享产物, arm64/x86_64 同一份) 里已含 GALLIUM_DRIVER=virpipe
        // 与 LIBGL_DRIVERS_PATH=$ORIGIN/lib/dri, 而 getenv 取首个匹配项,
        // push_back 追加的 softpipe/el1 值会被产物里的旧值遮蔽。
        UpsertEnvLine(env, "GALLIUM_DRIVER=softpipe");
        UpsertEnvLine(env, "LIBGL_DRIVERS_PATH=/data/storage/el1/bundle/libs/x86_64");
#else
        if (!state.virglSocketPath.empty()) env.push_back("VTEST_SOCKET_NAME=" + state.virglSocketPath);
#endif
    }
}

const char* GraphicsBroker::BackendName(GraphicsBackend backend)
{
    switch (backend) {
    case GraphicsBackend::Virgl:
        return "virgl";
    case GraphicsBackend::Shm:
    default:
        return "shm";
    }
}

bool GraphicsBroker::ParseBackendName(const std::string& name, GraphicsBackend* outBackend)
{
    if (!outBackend) return false;

    std::string lower = ToLower(name);
    if (lower == "shm") {
        *outBackend = GraphicsBackend::Shm;
        return true;
    }
    if (lower == "virgl") {
        *outBackend = GraphicsBackend::Virgl;
        return true;
    }
    return false;
}

bool GraphicsBroker::EnsureRuntimeLocked(const std::string& runtimeDir)
{
    if (!EnsureDir(runtimeDir)) return false;

    const std::string nextRuntimeDir = runtimeDir + "/graphics";
    runtimeDir_ = nextRuntimeDir;
    virglSocketPath_ = runtimeDir_ + "/virgl.sock";
    runtimeReady_ = EnsureDir(runtimeDir_);
    return runtimeReady_;
}

bool GraphicsBroker::IsVirglServerProcessAliveLocked()
{
    if (virglServerUsesInProcess_.load(std::memory_order_acquire))
    {
        if (virglServerRunning_.load(std::memory_order_acquire)) return true;
        lastError_ = "phone in-process virgl host is not running";
        virglServerUsesInProcess_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return false;
    }
    if (virglServerUsesIpc_)
    {
        std::lock_guard<std::mutex> lock(virglIpcMutex_);
        bool responsive = false;
        if (virglRemoteProxy_ && VirglProxyIsRemoteDead(virglRemoteProxy_) == 0)
        {
            OHIPCParcel* request = OH_IPCParcel_Create();
            OHIPCParcel* reply = OH_IPCParcel_Create();
            int32_t result = request
                ? OH_IPCParcel_WriteInt32(request, virgl_ipc::kProtocolVersion)
                : OH_IPC_MEM_ALLOCATOR_ERROR;
            if (result == OH_IPC_SUCCESS && reply)
            {
                OH_IPC_MessageOption option = {OH_IPC_REQUEST_MODE_SYNC, 0, nullptr};
                result = SendVirglRequestLocked(
                    virglRemoteProxy_, virgl_ipc::kQuerySurfacesRequest,
                    request, reply, &option);
            }
            responsive = result == OH_IPC_SUCCESS;
            if (reply) OH_IPCParcel_Destroy(reply);
            if (request) OH_IPCParcel_Destroy(request);
        }
        if (responsive) return true;

        lastError_ = "virgl IPC native child process is not responding";
        if (virglRemoteProxy_)
        {
            VirglProxyDestroy(virglRemoteProxy_);
            virglRemoteProxy_ = nullptr;
        }
        virglIpcConfigured_ = false;
        virglIpcCallbackComplete_ = false;
        virglServerUsesIpc_ = false;
        virglServerRunning_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return false;
    }
    if (virglServerUsesNcp_)
    {
        if (IsProcessRunningBySignal(virglServerPid_)) return true;

        lastError_ = "virgl native child process is not running";
        virglServerPid_ = -1;
        virglServerUsesNcp_ = false;
        virglServerRunning_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return false;
    }
    int status = 0;
    pid_t waited = 0;

    if (virglServerPid_ <= 0) return false;

    waited = waitpid(virglServerPid_, &status, WNOHANG);
    if (waited == 0) return true;

    if (waited == virglServerPid_)
    {
        std::string statusText = DescribeWaitStatus(status);
        lastError_ = "virgl_test_server terminated: " + statusText;
        OH_LOG_WARN(LOG_APP,
                    "[GraphicsBroker] virgl_test_server pid=%{public}d exited before guest connection (%{public}s)",
                    virglServerPid_, statusText.c_str());
    }
    virglServerPid_ = -1;
    virglServerUsesNcp_ = false;
    virglServerRunning_.store(false, std::memory_order_release);
    virglSocketReady_ = false;
    return false;
}

void GraphicsBroker::RefreshVirglStateLocked()
{
    bool loaded = false;
    virglLibraryPath_ = ProbeVirglLibraryLocked(&loaded);
    virglLibraryPresent_ = loaded;
    if (!virglLibraryPresent_) lastError_ = "virglrenderer library not found; using shm fallback";
}

void GraphicsBroker::RefreshGuestReceiverStateLocked()
{
    std::string receiverDir;
    std::string envPath;
    std::string libDir;
    std::vector<std::string> envLines;
    std::string mode;
    bool hasLibEGL = false;
    bool hasClientApi = false;
    bool hasDriverPayload = false;

    guestReceiverPresent_ = false;
    guestReceiverRuntimeDir_.clear();
    guestReceiverMode_.clear();
    guestReceiverError_.clear();
    guestReceiverEnv_.clear();

    if (wineRuntimeBinDir_.empty()) return;

    receiverDir = wineRuntimeBinDir_ + "/" + GUEST_GFX_DIRNAME;
    envPath = receiverDir + "/" + GUEST_GFX_ENVFILE;
    if (!FileExists(envPath))
    {
        guestReceiverError_ = "guest receiver env missing: " + envPath;
        return;
    }
    if (!LoadGuestReceiverEnvFile(receiverDir, envPath, envLines, mode))
    {
        guestReceiverError_ = "failed to parse guest receiver env: " + envPath;
        return;
    }

    guestReceiverRuntimeDir_ = receiverDir;
    guestReceiverMode_ = mode.empty() ? "external-bundle" : mode;

    libDir = receiverDir + "/lib";
    if (!DirExists(libDir))
    {
        guestReceiverError_ = "guest receiver lib dir missing: " + libDir;
        return;
    }

    hasLibEGL = FileExists(libDir + "/libEGL.so") || FileExists(libDir + "/libEGL.so.1");
    if (!hasLibEGL)
    {
        guestReceiverError_ = "guest receiver is missing libEGL.so* in " + libDir;
        return;
    }

    hasClientApi = FileExists(libDir + "/libGL.so") || FileExists(libDir + "/libGL.so.1") ||
                   FileExists(libDir + "/libOpenGL.so") || FileExists(libDir + "/libOpenGL.so.0") ||
                   FileExists(libDir + "/libGLESv2.so") || FileExists(libDir + "/libGLESv2.so.2") ||
                   FileExists(libDir + "/libGLESv1_CM.so") || FileExists(libDir + "/libGLESv1_CM.so.1");
    if (!hasClientApi)
    {
        guestReceiverError_ = "guest receiver is missing libGL.so* or libGLESv2.so* in " + libDir;
        return;
    }

    hasDriverPayload = DirExists(libDir + "/dri") || DirExists(libDir + "/egl") || DirExists(libDir + "/gallium") ||
                       FileExists(libDir + "/libgallium_dri.so") ||
                       DirHasSharedObjectWithPrefix(libDir, "libgallium-");
    if (!hasDriverPayload)
    {
        guestReceiverError_ = "guest receiver is missing Mesa driver payloads (dri/egl/gallium/libgallium-*.so) in " + libDir;
        return;
    }

    if (mode.find("virpipe") != std::string::npos && !FileExists(libDir + "/dri/virtio_gpu_dri.so"))
    {
        guestReceiverError_ = "guest receiver is missing lib/dri/virtio_gpu_dri.so in " + libDir;
        return;
    }

    guestReceiverPresent_ = true;
    guestReceiverEnv_ = std::move(envLines);

    OH_LOG_INFO(LOG_APP,
                "[GraphicsBroker] guest 3D receiver bundle detected mode=%{public}s dir=%{public}s",
                guestReceiverMode_.c_str(),
                guestReceiverRuntimeDir_.c_str());
}

void GraphicsBroker::StartVirglSocketServerLocked()
{
    std::string serverDir;
    std::string ldLibraryPath;

    if (!runtimeReady_ || virglSocketPath_.empty()) return;
    if (wineRuntimeBinDir_.empty())
    {
        lastError_ = "wine runtime bin dir is not configured; using shm fallback";
        return;
    }

    if (virglVtestLibraryPath_.empty() || !FileExists(virglVtestLibraryPath_))
    {
        lastError_ = "virgl vtest helper is missing from the bundle";
        return;
    }

    serverDir = DirNameCopy(virglVtestLibraryPath_);
    ldLibraryPath = serverDir;
    {
        const char* requestedSyncMode = getenv("WINEHUA_VIRGL_SYNC_MODE");
        std::string syncMode = requestedSyncMode ? requestedSyncMode : "egl-main";
        if (syncMode != "egl-thread" && syncMode != "egl-main" && syncMode != "native-fd")
        {
            OH_LOG_WARN(LOG_APP, "[GraphicsBroker] invalid sync mode %{public}s; using egl-thread",
                        syncMode.c_str());
            syncMode = "egl-thread";
        }
        const char* requestedLogPath = getenv("WINEHUA_VIRGL_HOST_LOG_PATH");
        const std::string virglLogPath = requestedLogPath && requestedLogPath[0]
            ? requestedLogPath
            : "/data/storage/el2/base/cache/winehua_virgl_host.log";
        const bool phoneMode = PhoneAdapter_IsPhoneMode();
        const char* requestedShadowMode =
            getenv("WINEHUA_VIRGL_HOST_SHADOW_MODE");
        if (!requestedShadowMode || !requestedShadowMode[0])
            requestedShadowMode = getenv("VKR_WINEHUA_SHADOW_FROM_HOST");
        const char* requestedShadowTrace =
            getenv("WINEHUA_VIRGL_HOST_SHADOW_SELECTOR");
        if (!requestedShadowTrace || !requestedShadowTrace[0])
            requestedShadowTrace = getenv("VKR_WINEHUA_SHADOW_TRACE");
        const char* requestedPresentMode =
            getenv("WINEHUA_VIRGL_HOST_PRESENT_MODE");
        if (!requestedPresentMode || !requestedPresentMode[0])
            requestedPresentMode = getenv("WINEHUA_VENUS_PRESENT_MODE");
        const std::string shadowMode = requestedShadowMode && requestedShadowMode[0]
            ? requestedShadowMode : "full";
        const std::string shadowTrace = requestedShadowTrace && requestedShadowTrace[0]
            ? requestedShadowTrace : "0";
        const std::string presentMode = requestedPresentMode &&
            (!strcmp(requestedPresentMode, "mailbox") ||
             !strcmp(requestedPresentMode, "fifo-async") ||
             !strcmp(requestedPresentMode, "fifo-poll"))
            ? requestedPresentMode : "fifo";
        const char* requestedMergeRanges =
            getenv("WINEHUA_VIRGL_HOST_SHADOW_MERGE_RANGES");
        if (!requestedMergeRanges || !requestedMergeRanges[0])
            requestedMergeRanges = getenv("VKR_WINEHUA_SHADOW_MERGE_RANGES");
        const char* requestedDescriptorSerialize =
            getenv("WINEHUA_VIRGL_HOST_DESCRIPTOR_UPDATE_SERIALIZE");
        if (!requestedDescriptorSerialize || !requestedDescriptorSerialize[0])
            requestedDescriptorSerialize =
                getenv("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE");
        const char* requestedGpuUploadWait =
            getenv("WINEHUA_VIRGL_HOST_GPU_UPLOAD_WAIT");
        if (!requestedGpuUploadWait || !requestedGpuUploadWait[0])
            requestedGpuUploadWait = getenv("VKR_WINEHUA_GPU_UPLOAD_WAIT");
        VirglHostConfig desiredConfig = {
            virglVtestLibraryPath_, virglSocketPath_, ldLibraryPath, syncMode,
            virglLogPath, shadowMode, shadowTrace, presentMode,
            requestedMergeRanges && requestedMergeRanges[0]
                ? requestedMergeRanges : "1",
            requestedDescriptorSerialize && requestedDescriptorSerialize[0]
                ? requestedDescriptorSerialize : "0",
            requestedGpuUploadWait && requestedGpuUploadWait[0]
                ? requestedGpuUploadWait : "0",
        };
        std::string configError;
        if (!ValidateVirglHostConfig(desiredConfig, &configError))
        {
            lastError_ = "invalid VirGL host configuration: " + configError;
            OH_LOG_ERROR(LOG_APP, "[GraphicsBroker] %{public}s", lastError_.c_str());
            return;
        }
        const uint64_t desiredConfigHash = FingerprintVirglHostConfig(desiredConfig);
        if (virglServerRunning_.load(std::memory_order_acquire) &&
            IsVirglServerProcessAliveLocked())
        {
            virglSocketReady_ = FileExists(virglSocketPath_);
            if (virglHostConfigHash_ != 0 && virglHostConfigHash_ != desiredConfigHash)
            {
                lastError_ = "VirGL host configuration changed after startup; App restart required";
                OH_LOG_ERROR(LOG_APP,
                             "[GraphicsBroker] host config stale active=0x%{public}llx desired=0x%{public}llx",
                             static_cast<unsigned long long>(virglHostConfigHash_),
                             static_cast<unsigned long long>(desiredConfigHash));
            }
            else if (!virglSocketReady_)
            {
                lastError_ = "virgl host is still starting; waiting for vtest socket";
            }
            return;
        }
        unlink(virglSocketPath_.c_str());
        {
            std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
            virglHostConfig_ = desiredConfig;
            virglHostConfigHash_ = desiredConfigHash;
            virglIpcAcceptCallback_ = !phoneMode;
            virglIpcCallbackComplete_ = false;
            virglIpcConfigured_ = false;
            virglIpcError_ = 0;
        }

        if (phoneMode)
        {
            if (!StartVirglInProcessHostLocked(desiredConfig))
            {
                virglServerRunning_.store(false, std::memory_order_release);
                virglSocketReady_ = false;
                return;
            }
            OH_LOG_INFO(LOG_APP,
                        "[GraphicsBroker] phone in-process VirGL host configured "
                        "helper=%{public}s socket=%{public}s hostLib=%{public}s config=0x%{public}llx",
                        virglVtestLibraryPath_.c_str(), virglSocketPath_.c_str(),
                        ldLibraryPath.c_str(),
                        static_cast<unsigned long long>(desiredConfigHash));
        }
        else
        {
            const int32_t ret = OH_Ability_CreateNativeChildProcess(
                "libvirgl_child.so", &GraphicsBroker::OnVirglIpcProcessStarted);
            if (ret != NCP_NO_ERROR)
            {
                std::lock_guard<std::mutex> ipcLock(virglIpcMutex_);
                virglIpcAcceptCallback_ = false;
                lastError_ = "failed to create virgl IPC native child process ret=" + std::to_string(ret);
                virglServerRunning_.store(false, std::memory_order_release);
                virglSocketReady_ = false;
                return;
            }

            std::unique_lock<std::mutex> ipcLock(virglIpcMutex_);
            const bool callbackCompleted = virglIpcCondition_.wait_for(
                ipcLock, std::chrono::seconds(5), [this]() { return virglIpcCallbackComplete_; });
            if (!callbackCompleted || !virglIpcConfigured_)
            {
                virglIpcAcceptCallback_ = false;
                lastError_ = callbackCompleted
                    ? "failed to configure virgl IPC native child process ret=" +
                        std::to_string(virglIpcError_)
                    : "timed out waiting for virgl IPC native child process";
                ipcLock.unlock();
                ShutdownVirglIpc();
                virglServerRunning_.store(false, std::memory_order_release);
                virglSocketReady_ = false;
                return;
            }
            ipcLock.unlock();

            virglServerUsesIpc_ = true;
            virglServerUsesNcp_ = false;
            virglServerUsesInProcess_.store(false, std::memory_order_release);
            virglServerRunning_.store(true, std::memory_order_release);
            OH_LOG_INFO(LOG_APP,
                        "[GraphicsBroker] IPC NCP virgl_child configured helper=%{public}s "
                        "socket=%{public}s hostLib=%{public}s sync=%{public}s log=%{public}s "
                        "present=%{public}s config=0x%{public}llx",
                        virglVtestLibraryPath_.c_str(), virglSocketPath_.c_str(),
                        ldLibraryPath.c_str(), syncMode.c_str(), virglLogPath.c_str(),
                        presentMode.c_str(),
                        static_cast<unsigned long long>(desiredConfigHash));
        }
    }

    if (!virglServerUsesInProcess_.load(std::memory_order_acquire)) virglServerPid_ = -1;
    virglSocketReady_ = false;

    OH_LOG_INFO(LOG_APP, "[GraphicsBroker] waiting for virgl socket at %{public}s",
                virglSocketPath_.c_str());

    if (WaitFor("virgl_test_server socket",
                [this]() { return FileExists(virglSocketPath_) || !IsVirglServerProcessAliveLocked(); },
                4000, 100) &&
        FileExists(virglSocketPath_) &&
        IsVirglServerProcessAliveLocked())
    {
        virglSocketReady_ = true;
        lastError_ = "VirGL vtest server is up; waiting for guest-side 3D receiver";
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] virgl_test_server pid=%{public}d listening at %{public}s",
                    virglServerPid_, virglSocketPath_.c_str());
        return;
    }

    const bool socketExists = FileExists(virglSocketPath_);
    const bool serverAlive = IsVirglServerProcessAliveLocked();
    OH_LOG_ERROR(LOG_APP,
                 "[GraphicsBroker] virgl socket wait FAILED: socket_exists=%{public}d process_alive=%{public}d",
                 socketExists ? 1 : 0, serverAlive ? 1 : 0);

    // An in-process host cannot be killed independently from the application.
    // Keep its real state so a later Prepare() can observe a delayed socket.
    if (virglServerUsesInProcess_.load(std::memory_order_acquire) && serverAlive)
    {
        virglSocketReady_ = false;
        lastError_ = "timed out waiting for virgl_test_server socket; host is still starting";
        return;
    }

    if (virglServerPid_ > 0 &&
        !virglServerUsesInProcess_.load(std::memory_order_acquire))
    {
        TerminateTrackedProcess(virglServerPid_, virglServerUsesNcp_);
    }
    if (virglServerUsesIpc_) ShutdownVirglIpc();
    virglServerPid_ = -1;
    virglServerUsesNcp_ = false;
    virglServerUsesIpc_ = false;
    virglServerUsesInProcess_.store(false, std::memory_order_release);
    virglServerRunning_.store(false, std::memory_order_release);
    virglSocketReady_ = false;
    lastError_ = "timed out waiting for virgl_test_server socket";
}

void GraphicsBroker::UpdateActiveBackendLocked()
{
    if (requestedBackend_ == GraphicsBackend::Shm) {
        activeBackend_ = GraphicsBackend::Shm;
        loggedVirglFallback_ = false;
        return;
    }

    if (runtimeReady_ && virglLibraryPresent_ && virglSocketReady_ && guestReceiverPresent_)
    {
        activeBackend_ = GraphicsBackend::Virgl;
        loggedVirglFallback_ = false;
        lastError_.clear();
        return;
    }

    activeBackend_ = GraphicsBackend::Shm;
    if (!runtimeReady_) {
        lastError_ = "graphics runtime is not ready; using shm fallback";
    } else if (!virglLibraryPresent_) {
        lastError_ = "virglrenderer library not found; using shm fallback";
    } else if (!virglSocketReady_) {
        lastError_ = "virgl socket is not ready yet; using shm fallback";
    } else if (!guestReceiverPresent_) {
        if (!guestReceiverError_.empty()) {
            lastError_ = "virgl host is ready, but guest receiver bundle is incomplete: " + guestReceiverError_ +
                         "; Windows OpenGL/DX still uses stock wayland/EGL; using shm fallback";
        } else {
            lastError_ = "virgl host is ready, but no guest 3D receiver bundle was staged "
                         "(guest_gfx/winehua-guest-gfx.env missing); Windows OpenGL/DX still uses stock wayland/EGL; using shm fallback";
        }
    } else {
        lastError_ = "VirGL runtime prerequisites are not satisfied yet; using shm fallback";
    }

    if (!loggedVirglFallback_) {
        OH_LOG_WARN(LOG_APP, "[GraphicsBroker] requested backend=%{public}s active=%{public}s reason=%{public}s",
                    BackendName(requestedBackend_), BackendName(activeBackend_), lastError_.c_str());
        loggedVirglFallback_ = true;
    }
}

std::string GraphicsBroker::ProbeVirglLibraryLocked(bool* outLoaded) const
{
    const char* envLib = std::getenv("WINEHUA_VIRGLRENDERER_LIB");
    const char* candidates[] = {
        envLib && envLib[0] ? envLib : nullptr,
        "libvirglrenderer.so",
        "libvirglrenderer.so.1",
    };

    if (outLoaded) *outLoaded = false;

    for (const char* candidate : candidates) {
        if (!candidate || !candidate[0]) continue;

        void* handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;

        void* symbol = dlsym(handle, "virgl_renderer_init");
        dlclose(handle);
        if (!symbol) continue;

        if (outLoaded) *outLoaded = true;
        return candidate;
    }

    return "";
}

} // namespace winehua
