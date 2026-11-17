#pragma once

#include "input/controller/controller_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winehua {
namespace controller {

// AF_UNIX WHGP server. Hub state listener pushes snapshots to connected winebus
// clients; rumble packets from winebus are forwarded to a JS/native listener.
class GamepadBridge {
public:
    using RumbleListener = std::function<void(uint32_t slot, uint16_t low, uint16_t high, uint32_t durationMs)>;

    static GamepadBridge& Instance();

    bool Start(const std::string& socketPath);
    void Stop();
    bool IsRunning() const;
    std::string SocketPath() const;
    void PublishState(uint32_t slot, const LogicalGamepadState& state);
    void AttachToHub();
    void SetRumbleListener(RumbleListener cb);

private:
    GamepadBridge() = default;
    ~GamepadBridge();
    struct Client;
    void StopLocked();
    void AcceptLoop();
    void RecvLoop(const std::shared_ptr<Client>& client);
    void WriteState(const std::shared_ptr<Client>& client, uint32_t slot,
                    const LogicalGamepadState& state);

    std::mutex lifecycleMutex_;
    mutable std::mutex mutex_;
    std::string path_;
    int listenFd_ = -1;
    std::vector<std::shared_ptr<Client>> clients_;
    bool running_ = false;
    std::thread acceptThread_;
    RumbleListener rumbleListener_;
};

}  // namespace controller
}  // namespace winehua
