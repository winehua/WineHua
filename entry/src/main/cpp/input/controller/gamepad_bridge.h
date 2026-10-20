#pragma once

#include "input/controller/controller_types.h"

#include <chrono>
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
    void LogThrottled(const char* what, int fd, pid_t pid);

    mutable std::mutex mutex_;
    std::string path_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    pid_t clientPid_ = -1;  // 当前连接对端 pid; 同 pid 才允许接管旧连接
    bool running_ = false;
    std::thread acceptThread_;
    std::thread rumbleThread_;
    RumbleListener rumbleListener_;
    std::chrono::steady_clock::time_point lastConnectLog_{};  // connected/exited 日志节流
};

}  // namespace controller
}  // namespace winehua
