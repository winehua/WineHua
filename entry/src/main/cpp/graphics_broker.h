#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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
    bool virglSmokeAttempted = false;
    bool virglSmokeSucceeded = false;
    bool zeroCopyFramePath = false;
    std::string runtimeDir;
    std::string guestReceiverRuntimeDir;
    std::string guestReceiverMode;
    std::string guestReceiverError;
    std::string virglSocketPath;
    std::string virglLibraryPath;
    std::string frameTransportMode;
    std::string virglSmokeError;
    std::string lastError;
};

class GraphicsBroker
{
public:
    static GraphicsBroker& GetInstance();

    bool EnsureStarted(const std::string& runtimeDir);
    void Stop();
    void SetWineRuntimeBinaryDir(const std::string& wineBinDir);

    void SetRequestedBackend(GraphicsBackend backend);
    GraphicsBackendState GetState() const;

    void AppendWineEnv(std::vector<std::string>& env) const;
    bool TakeFrameForToplevel(uint32_t rendererToplevelId,
                              std::vector<uint8_t>& outPixels,
                              int& w,
                              int& h,
                              uint32_t* outSourceToplevelId = nullptr);

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
    void ProbeVirglRuntimeSmokeLocked();
    void StartVirglSocketServerLocked();
    void UpdateActiveBackendLocked();
    std::string ProbeVirglLibraryLocked(bool* outLoaded) const;

    mutable std::mutex mutex_;
    GraphicsBackend requestedBackend_ = GraphicsBackend::Shm;
    GraphicsBackend activeBackend_ = GraphicsBackend::Shm;
    bool started_ = false;
    bool runtimeReady_ = false;
    bool guestReceiverPresent_ = false;
    bool virglSocketReady_ = false;
    bool virglLibraryPresent_ = false;
    bool virglSmokeAttempted_ = false;
    bool virglSmokeSucceeded_ = false;
    bool loggedVirglFallback_ = false;
    std::string runtimeDir_;
    std::string wineRuntimeBinDir_;
    std::string guestReceiverRuntimeDir_;
    std::string guestReceiverMode_;
    std::string guestReceiverError_;
    std::vector<std::string> guestReceiverEnv_;
    std::string virglServerProgramPath_;
    std::string virglSocketPath_;
    std::string virglLibraryPath_;
    std::string virglSmokeError_;
    std::string lastError_;
    int virglServerPid_ = -1;
    std::atomic<bool> virglServerRunning_{false};
};

} // namespace winehua
