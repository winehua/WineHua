#include "spawn_codec.h"
// CreateNativeChildProcess bootstrap for a future opt-in Direct Wine path.
// The existing StartNativeChildProcess Main(NativeChildProcess_Args) entry is unchanged.
#include "wine_child_ipc.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <hilog/log.h>
#include <native_window/external_window.h>
#include <unistd.h>
#include <dlfcn.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#define LOG_DOMAIN 0x2330
#define LOG_TAG "WineChildIPC"

extern "C" void Main(NativeChildProcess_Args args);

namespace {

struct NamedFd {
    std::string name;
    int fd = -1;
};

std::mutex g_mutex;
std::condition_variable g_ready;
bool g_received = false;
std::string g_params;
std::vector<NamedFd> g_fds;

struct DirectSurface {
    winehua::wineipc::DirectSurfaceToken token{};
    OHNativeWindow* window = nullptr;
};
std::mutex g_surfaceMutex;
std::unordered_map<uint32_t, DirectSurface> g_surfaces;
std::unordered_map<uint32_t, uint64_t> g_lastGeneration;
std::mutex g_surfaceProbeMutex;
std::condition_variable g_surfaceProbeCondition;
bool g_surfaceProbeFinished = false;

bool ReadSurfaceToken(const OHIPCParcel* request, winehua::wineipc::DirectSurfaceToken* token)
{
    int32_t toplevelId = 0, wlSurfaceId = 0;
    int64_t generation = 0;
    if (OH_IPCParcel_ReadInt32(request, &token->clientPid) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &toplevelId) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &wlSurfaceId) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt64(request, &generation) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &token->width) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &token->height) != OH_IPC_SUCCESS)
        return false;
    token->toplevelId = static_cast<uint32_t>(toplevelId);
    token->wlSurfaceId = static_cast<uint32_t>(wlSurfaceId);
    token->generation = static_cast<uint64_t>(generation);
    return token->clientPid == getpid() && token->toplevelId && token->wlSurfaceId &&
           generation > 0 && token->width > 0 && token->height > 0;
}

int OnSurfaceRequest(uint32_t code, const OHIPCParcel* request, OHIPCParcel* reply)
{
    winehua::wineipc::DirectSurfaceToken token{};
    if (!ReadSurfaceToken(request, &token)) return OH_IPC_CHECK_PARAM_ERROR;
    OHNativeWindow* incoming = nullptr;
    int32_t result = -1;
    {
        std::lock_guard<std::mutex> lock(g_surfaceMutex);
        const uint64_t last = g_lastGeneration[token.wlSurfaceId];
        auto it = g_surfaces.find(token.wlSurfaceId);
        if (code == winehua::wineipc::kAttachSurface) {
            if (token.generation > last) {
                // Reject stale messages before deserializing their producer.
                // On this device ReadFromParcel may return the same native
                // object as an existing lease; destroying a rejected duplicate
                // invalidates the live lease despite the separate parcel.
                if (OH_NativeWindow_ReadFromParcel(const_cast<OHIPCParcel*>(request), &incoming) == 0 &&
                    incoming) {
                    if (it != g_surfaces.end()) {
                        OH_NativeWindow_DestroyNativeWindow(it->second.window);
                        g_surfaces.erase(it);
                    }
                    g_surfaces[token.wlSurfaceId] = {token, incoming};
                    g_lastGeneration[token.wlSurfaceId] = token.generation;
                    incoming = nullptr;
                    result = 0;
                }
            } else if (token.generation == last && it != g_surfaces.end() &&
                       it->second.token.toplevelId == token.toplevelId &&
                       it->second.token.width == token.width &&
                       it->second.token.height == token.height) {
                result = 0; // retry after an ambiguous IPC reply
            }
        } else if (code == winehua::wineipc::kDetachSurface) {
            if (it != g_surfaces.end() && it->second.token.generation == token.generation &&
                it->second.token.toplevelId == token.toplevelId) {
                OH_NativeWindow_DestroyNativeWindow(it->second.window);
                g_surfaces.erase(it);
                result = 0;
            }
        } else if (code == winehua::wineipc::kResizeSurface) {
            // Size metadata changes without invalidating a VkSurfaceKHR that
            // already borrowed this producer. The generation identifies the
            // queue, not each xdg configure/resize event.
            if (it != g_surfaces.end() && it->second.token.generation == token.generation &&
                it->second.token.toplevelId == token.toplevelId) {
                it->second.token.width = token.width;
                it->second.token.height = token.height;
                result = 0;
            }
        } else if (code == winehua::wineipc::kQuerySurface) {
            if (it != g_surfaces.end() && it->second.token.generation == token.generation &&
                it->second.token.toplevelId == token.toplevelId &&
                OH_NativeWindow_NativeObjectReference(it->second.window) == 0) {
                OH_NativeWindow_NativeObjectUnreference(it->second.window);
                result = 0;
            }
        }
    }
    OH_LOG_INFO(LOG_APP,
                "[DIRECT-D3][NCP] surface op=%{public}u pid=%{public}d top=%{public}u wl=%{public}u gen=%{public}llu result=%{public}d",
                code, token.clientPid, token.toplevelId, token.wlSurfaceId,
                static_cast<unsigned long long>(token.generation), result);
    if (incoming) OH_NativeWindow_DestroyNativeWindow(incoming);
    return OH_IPCParcel_WriteInt32(reply, result);
}

void CloseFds(std::vector<NamedFd>& fds)
{
    for (auto& item : fds) {
        if (item.fd >= 0) close(item.fd);
        item.fd = -1;
    }
}

int OnRequest(uint32_t code, const OHIPCParcel* request, OHIPCParcel* reply, void*)
{
    if (!request || !reply)
        return OH_IPC_CHECK_PARAM_ERROR;

    int32_t version = 0;
    int32_t count = -1;
    if (OH_IPCParcel_ReadInt32(request, &version) != OH_IPC_SUCCESS ||
        version != winehua::wineipc::kVersion)
        return OH_IPC_CHECK_PARAM_ERROR;
    if (code == winehua::wineipc::kFinishSurfaceProbe) {
        {
            std::lock_guard<std::mutex> lock(g_surfaceProbeMutex);
            g_surfaceProbeFinished = true;
        }
        g_surfaceProbeCondition.notify_one();
        return OH_IPCParcel_WriteInt32(reply, 0);
    }
    if (code == winehua::wineipc::kAttachSurface ||
        code == winehua::wineipc::kDetachSurface ||
        code == winehua::wineipc::kResizeSurface ||
        code == winehua::wineipc::kQuerySurface)
        return OnSurfaceRequest(code, request, reply);
    if (code != winehua::wineipc::kBootstrap)
        return OH_IPC_CHECK_PARAM_ERROR;
    const char* params = OH_IPCParcel_ReadString(request);
    if (!params || strnlen(params, VP_ENTRY_CAP) >= VP_ENTRY_CAP ||
        OH_IPCParcel_ReadInt32(request, &count) != OH_IPC_SUCCESS ||
        count < 0 || count > winehua::wineipc::kMaxFds)
        return OH_IPC_CHECK_PARAM_ERROR;

    std::vector<NamedFd> received;
    received.reserve(count);
    for (int32_t i = 0; i < count; ++i) {
        const char* name = OH_IPCParcel_ReadString(request);
        int32_t fd = -1;
        if (!name || !name[0] || std::strlen(name) > 20 ||
            OH_IPCParcel_ReadFileDescriptor(request, &fd) != OH_IPC_SUCCESS || fd < 0) {
            CloseFds(received);
            return OH_IPC_CHECK_PARAM_ERROR;
        }
        received.push_back({name, fd});
    }

    if (!winehua::wineipc::IsProbeParams(params)) {
        winehua::spawn::Request startup;
        std::vector<std::string> actualNames;
        for (const auto& item : received) actualNames.push_back(item.name);
        bool valid = winehua::spawn::DecodeEntry(params, startup) && !startup.home.empty() &&
            winehua::spawn::MatchNames(startup, actualNames);
        if (!valid) { CloseFds(received); return OH_IPC_CHECK_PARAM_ERROR; }
    }

    // Do not release MainProc until the PID reply is ready. A short-lived
    // child could otherwise exit before SendRequest receives its response.
    if (OH_IPCParcel_WriteInt32(reply, getpid()) != OH_IPC_SUCCESS) {
        CloseFds(received);
        return OH_IPC_CHECK_PARAM_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_received) {
            CloseFds(received);
            return OH_IPC_CHECK_PARAM_ERROR;
        }
        g_params = params;
        g_fds = std::move(received);
        g_received = true;
    }
    g_ready.notify_one();
    return OH_IPC_SUCCESS;
}

void RunProbe(std::vector<NamedFd>& fds)
{
    int input = -1;
    int output = -1;
    for (const auto& item : fds) {
        if (item.name == "probe_input") input = item.fd;
        if (item.name == "probe_output") output = item.fd;
    }
    winehua::wineipc::ProbeResult result{getpid(), -1, 0, static_cast<int32_t>(fds.size())};
    if (input >= 0 && output >= 0 && fds.size() >= 2 &&
        read(input, &result.token, sizeof(result.token)) == sizeof(result.token) &&
        result.token == winehua::wineipc::kProbeToken)
        result.status = 0;
    if (output >= 0) (void)write(output, &result, sizeof(result));
    CloseFds(fds);
}

} // namespace

// The Wine Vulkan WSI borrows a native-object reference. The caller releases
// it via WineHua_DirectSurfaceRelease after destroying its VkSurfaceKHR.
extern "C" __attribute__((visibility("default"))) OHNativeWindow*
WineHua_DirectSurfaceAcquire(uint32_t toplevelId, uint32_t wlSurfaceId,
                              uint64_t* generation, int32_t* width, int32_t* height)
{
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    auto it = g_surfaces.find(wlSurfaceId);
    if (it == g_surfaces.end() || it->second.token.toplevelId != toplevelId ||
        OH_NativeWindow_NativeObjectReference(it->second.window) != 0)
        return nullptr;
    if (generation) *generation = it->second.token.generation;
    if (width) *width = it->second.token.width;
    if (height) *height = it->second.token.height;
    return it->second.window;
}

extern "C" __attribute__((visibility("default"))) OHNativeWindow*
WineHua_DirectSurfaceAcquireByWlSurface(uint32_t wlSurfaceId, uint32_t* toplevelId,
                                         uint64_t* generation, int32_t* width,
                                         int32_t* height)
{
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    auto it = g_surfaces.find(wlSurfaceId);
    if (it == g_surfaces.end() || OH_NativeWindow_NativeObjectReference(it->second.window) != 0)
        return nullptr;
    const auto& surface = it->second;
    if (toplevelId) *toplevelId = surface.token.toplevelId;
    if (generation) *generation = surface.token.generation;
    if (width) *width = surface.token.width;
    if (height) *height = surface.token.height;
    return surface.window;
}

extern "C" __attribute__((visibility("default"))) void
WineHua_DirectSurfaceRelease(OHNativeWindow* window)
{
    if (!window) return;
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    OH_NativeWindow_NativeObjectUnreference(window);
}

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    return OH_IPCRemoteStub_Create("winehua.direct.wine.bootstrap", OnRequest, nullptr, nullptr);
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    std::string params;
    std::vector<NamedFd> fds;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_ready.wait_for(lock, std::chrono::seconds(15), [] { return g_received; })) {
            OH_LOG_ERROR(LOG_APP, "[WineChildIPC] bootstrap timeout pid=%{public}d", getpid());
            return;
        }
        params = std::move(g_params);
        fds = std::move(g_fds);
    }
    if (params == winehua::wineipc::kProbeParams ||
        params == std::string(winehua::wineipc::kProbeParams) + "|__env=WINEHUA_DIRECT_NCP=1" ||
        [&] {
            winehua::spawn::Request startup;
            return winehua::spawn::DecodeEntry(params.c_str(), startup) &&
                startup.argv == std::vector<std::string>{winehua::wineipc::kProbeParams};
        }()) {
        RunProbe(fds);
        return;
    }
    if (params == winehua::wineipc::kSurfaceProbeParams ||
        params == std::string(winehua::wineipc::kSurfaceProbeParams) +
                  "|__env=WINEHUA_VULKAN_BACKEND=direct") {
        CloseFds(fds);
        std::unique_lock<std::mutex> lock(g_surfaceProbeMutex);
        g_surfaceProbeCondition.wait_for(lock, std::chrono::seconds(30),
                                         [] { return g_surfaceProbeFinished; });
        lock.unlock();
        std::lock_guard<std::mutex> surfaceLock(g_surfaceMutex);
        for (auto& [id, surface] : g_surfaces)
            OH_NativeWindow_DestroyNativeWindow(surface.window);
        g_surfaces.clear();
        return;
    }

    // Own storage for the complete Main call. fdName pointers refer to fds strings.
    std::vector<NativeChildProcess_Fd> nodes(fds.size());
    for (size_t i = 0; i < fds.size(); ++i) {
        nodes[i].fdName = const_cast<char*>(fds[i].name.c_str());
        nodes[i].fd = fds[i].fd;
        nodes[i].next = i + 1 < fds.size() ? &nodes[i + 1] : nullptr;
    }
    NativeChildProcess_Args args{};
    args.entryParams = const_cast<char*>(params.c_str());
    args.fdList.head = nodes.empty() ? nullptr : &nodes[0];
    if (params == winehua::wineipc::kBufferProbeParams) {
        // Qualify Vulkan under the same Create/IPC child context as Direct
        // games. The Start API's child cannot create a Vulkan instance here.
        void* probe = dlopen("libdirect_shared_buffer_probe.so", RTLD_NOW | RTLD_LOCAL);
        auto run = probe ? reinterpret_cast<void (*)(NativeChildProcess_Args)>(
            dlsym(probe, "Main")) : nullptr;
        if (!run) { CloseFds(fds); _exit(1); }
        run(args);
        _exit(1); // the standalone entry owns cleanup and exits itself
    }
    Main(args);
    // Main may close or hand off its fds; process exit releases any survivors.
}
