#pragma once

#include "input/controller/controller_types.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace winehua {
namespace controller {

// AF_UNIX WHGP server. Hub state listener pushes snapshots to connected winebus
// clients; rumble packets from winebus are forwarded to a JS/native listener.
class GamepadBridge {
public:
    using RumbleListener = std::function<void(uint16_t low, uint16_t high, uint32_t durationMs)>;

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
    void AcceptLoop();
    void RecvLoop(int fd);
    void WriteState(int fd, uint32_t slot, const LogicalGamepadState& state);

    mutable std::mutex mutex_;
    std::string path_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    bool running_ = false;
    std::thread acceptThread_;
    std::thread rumbleThread_;
    RumbleListener rumbleListener_;
};

}  // namespace controller
}  // namespace winehua
