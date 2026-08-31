#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <native_window/external_window.h>

#include "virgl_host_config.h"

struct OHIPCRemoteProxy;

namespace winehua {

enum class GraphicsBackend
{
    Shm = 0,
    Virgl = 1,
};

struct GraphicsBackendState
{
    GraphicsBackend requested = GraphicsBackend::Shm;
    GraphicsBackend active = GraphicsBackend::Shm;
    bool runtimeReady = false;
    bool guestReceiverPresent = false;
    bool virglSocketReady = false;
    bool virglLibraryPresent = false;
    bool zeroCopyFramePath = false;
    std::string runtimeDir;
    std::string guestReceiverRuntimeDir;
    std::string guestReceiverMode;
    std::string guestReceiverError;
    std::string virglSocketPath;
    std::string virglLibraryPath;
    std::string frameTransportMode;
    std::string lastError;
};

struct ZeroCopySurfaceInfo
{
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t surfaceId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;
    bool attached = false;
    bool vulkan = false;
};

class GraphicsBroker
{
public:
    static GraphicsBroker& GetInstance();

    bool EnsureStarted(const std::string& runtimeDir);
    void Stop();
    void SetWineRuntimeBinaryDir(const std::string& wineBinDir);

    void SetRequestedBackend(GraphicsBackend backend);
    void SetVulkanPresentMode(bool enabled);
    bool IsVulkanPresentMode() const;
    GraphicsBackendState GetState() const;

    void AppendWineEnv(std::vector<std::string>& env) const;
    bool AttachZeroCopyTarget(uint64_t surfaceKey, OHNativeWindow* producerWindow,
                              uint64_t framePeriodNs);
    void SetZeroCopyFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs);
    void DetachZeroCopyTarget(uint64_t surfaceKey);
    bool QueryZeroCopySurfaces(std::vector<ZeroCopySurfaceInfo>& surfaces) const;
    void SetZeroCopySurfaceReady(uint64_t surfaceKey, bool ready);

    static const char* BackendName(GraphicsBackend backend);
    static bool ParseBackendName(const std::string& name, GraphicsBackend* outBackend);

private:
    GraphicsBroker();
    GraphicsBroker(const GraphicsBroker&) = delete;
    GraphicsBroker& operator=(const GraphicsBroker&) = delete;

    bool EnsureRuntimeLocked(const std::string& runtimeDir);
    bool IsVirglServerProcessAliveLocked();
    void RefreshVirglStateLocked();
    void RefreshGuestReceiverStateLocked();
    void StartVirglSocketServerLocked();
    void UpdateActiveBackendLocked();
    std::string ProbeVirglLibraryLocked(bool* outLoaded) const;
    static void OnVirglIpcProcessStarted(int errorCode, OHIPCRemoteProxy* remoteProxy);
    bool SendVirglConfigureLocked();
    bool SendVirglTargetLocked(uint64_t surfaceKey, OHNativeWindow* producerWindow,
                               uint64_t framePeriodNs, uint32_t flags);
    bool SendVirglFramePeriodLocked(uint64_t surfaceKey, uint64_t framePeriodNs);
    bool SendVirglDetachLocked(uint64_t surfaceKey);
    bool StartVirglInProcessHostLocked(const VirglHostConfig& config);
    void ResetVirglInProcessSurfacesLocked();
    void ShutdownVirglIpc();

    mutable std::mutex mutex_;
    GraphicsBackend requestedBackend_ = GraphicsBackend::Shm;
    GraphicsBackend activeBackend_ = GraphicsBackend::Shm;
    bool started_ = false;
    bool runtimeReady_ = false;
    bool guestReceiverPresent_ = false;
    bool virglSocketReady_ = false;
    bool virglLibraryPresent_ = false;
    bool loggedVirglFallback_ = false;
    std::string runtimeDir_;
    std::string wineRuntimeBinDir_;
    std::string guestReceiverRuntimeDir_;
    std::string guestReceiverMode_;
    std::string guestReceiverError_;
    std::vector<std::string> guestReceiverEnv_;
    std::string virglServerProgramPath_;
    std::string virglVtestLibraryPath_;
    std::string virglSocketPath_;
    std::string virglLibraryPath_;
    std::string lastError_;
    int virglServerPid_ = -1;
    bool virglServerUsesNcp_ = false;
    bool virglServerUsesIpc_ = false;
    std::atomic<bool> virglServerUsesInProcess_{false};
    std::atomic<bool> virglServerRunning_{false};
    std::atomic<bool> vulkanPresentMode_{false};
    void* virglInProcessHandle_ = nullptr;
    void* virglInProcessStop_ = nullptr;
    void* virglInProcessAttach_ = nullptr;
    void* virglInProcessDetach_ = nullptr;
    void* virglInProcessSetFramePeriod_ = nullptr;
    void* virglInProcessQuery_ = nullptr;
    void* virglInProcessReset_ = nullptr;

    mutable std::mutex virglIpcMutex_;
    std::condition_variable virglIpcCondition_;
    OHIPCRemoteProxy* virglRemoteProxy_ = nullptr;
    bool virglIpcAcceptCallback_ = false;
    bool virglIpcCallbackComplete_ = false;
    bool virglIpcConfigured_ = false;
    int virglIpcError_ = 0;
    VirglHostConfig virglHostConfig_;
    uint64_t virglHostConfigHash_ = 0;
    std::unordered_set<uint64_t> zeroCopyAttachedSurfaces_;
};

} // namespace winehua
