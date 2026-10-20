#include "input/controller/gamepad_bridge.h"

#include "input/controller/controller_hub.h"
#include "input/controller/gamepad_ipc_protocol.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "CtrlHub"
#include <hilog/log.h>

namespace winehua {
namespace controller {

namespace {

bool ReadExact(int fd, void* buf, size_t len)
{
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        const ssize_t n = read(fd, p + got, len - got);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

GamepadBridge& GamepadBridge::Instance()
{
    static GamepadBridge bridge;
    return bridge;
}

std::string GamepadBridge::SocketPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

bool GamepadBridge::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void GamepadBridge::SetRumbleListener(RumbleListener cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rumbleListener_ = std::move(cb);
}

bool GamepadBridge::Start(const std::string& socketPath)
{
    std::string path = socketPath;
    if (path.empty()) {
        path = "/data/storage/el2/base/files/.wine/whgp.sock";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ && listenFd_ >= 0 && path_ == path) {
            OH_LOG_INFO(LOG_APP, "[WHGP] already listening on %{public}s", path.c_str());
            return true;
        }
    }
    Stop();

    unlink(path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[WHGP] socket failed errno=%{public}d", errno);
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        OH_LOG_ERROR(LOG_APP, "[WHGP] bind %{public}s failed errno=%{public}d", path.c_str(), errno);
        close(fd);
        return false;
    }
    chmod(path.c_str(), 0666);
    if (listen(fd, 1) != 0) {
        close(fd);
        unlink(path.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        listenFd_ = fd;
        path_ = path;
        running_ = true;
    }
    acceptThread_ = std::thread([this] { AcceptLoop(); });
    OH_LOG_INFO(LOG_APP, "[WHGP] listening on %{public}s", path.c_str());
    return true;
}

void GamepadBridge::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
            listenFd_ = -1;
        }
        if (clientFd_ >= 0) {
            shutdown(clientFd_, SHUT_RDWR);
            clientFd_ = -1;
        }
        if (!path_.empty()) unlink(path_.c_str());
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    if (rumbleThread_.joinable()) rumbleThread_.join();
}

void GamepadBridge::LogThrottled(const char* what, int fd, pid_t pid)
{
    // 连接风暴时 accept/退出每秒数千次, 全量日志实测 9 小时刷出 750 万条把
    // hilog 冲爆; 统一节流到每秒最多一条。
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (now - lastConnectLog_ < std::chrono::seconds(1)) return;
        lastConnectLog_ = now;
    }
    if (pid >= 0)
        OH_LOG_INFO(LOG_APP, "[WHGP] %{public}s fd=%{public}d peer_pid=%{public}d", what, fd, pid);
    else
        OH_LOG_INFO(LOG_APP, "[WHGP] %{public}s fd=%{public}d (no peercred)", what, fd);
}

void GamepadBridge::AcceptLoop()
{
    while (true) {
        int listenFd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            listenFd = listenFd_;
        }
        int client = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0 && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
            client = accept(listenFd, nullptr, nullptr);
            if (client >= 0) fcntl(client, F_SETFD, FD_CLOEXEC);
        }
        if (client < 0) {
            if (errno == EINTR) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            continue;
        }

        struct ucred peer{};
        socklen_t peerLen = sizeof(peer);
        const bool havePeer = getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peerLen) == 0;
        const pid_t peerPid = havePeer ? peer.pid : -1;

        int oldClient = -1;
        bool reject = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                close(client);
                return;
            }
            // 异 pid 来连且已有活跃连接 → 拒绝后者, 保护先到者: winedevice
            // 旧实例未退场时新实例接入会与旧实例互踢, 实测演变成每秒数千次
            // 的连接风暴; 只有同 pid 的重连才允许接管旧连接。
            if (clientFd_ >= 0 && peerPid >= 0 && clientPid_ != peerPid) {
                reject = true;
            } else {
                oldClient = clientFd_;
                clientFd_ = -1;
                clientPid_ = -1;
                if (oldClient >= 0) shutdown(oldClient, SHUT_RDWR);
            }
        }
        if (reject) {
            // LogThrottled 内部拿 mutex_, 与上方锁域分离 (不可重入)
            LogThrottled("client rejected", client, peerPid);
            close(client);
            continue;
        }
        if (rumbleThread_.joinable()) rumbleThread_.join();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                close(client);
                return;
            }
            clientFd_ = client;
            clientPid_ = peerPid;
        }
        // LogThrottled 内部也拿 mutex_, 不能在持锁状态调用 (std::mutex 不可
        // 重入, 否则 accept 线程自死锁并永久占锁, 后续所有 Start/PublishState
        // 等锁睡死 — 实测表现为 explorer 永不 spawn 的引擎死寂)。
        LogThrottled("client connected", client, peerPid);
        rumbleThread_ = std::thread([this, client] { RecvLoop(client); });
        PublishState(0, ControllerHub::Instance().GetState(0));
    }
}

void GamepadBridge::RecvLoop(int fd)
{
    while (true) {
        whgp_header hdr{};
        if (!ReadExact(fd, &hdr, sizeof(hdr))) break;
        if (hdr.magic != WHGP_MAGIC || hdr.version != WHGP_VERSION) {
            OH_LOG_WARN(LOG_APP, "[WHGP] bad header from winebus magic=%{public}u ver=%{public}u",
                        hdr.magic, hdr.version);
            break;
        }
        if (hdr.payload_size > 4096) {
            OH_LOG_WARN(LOG_APP, "[WHGP] payload too large %{public}u", hdr.payload_size);
            break;
        }

        if (hdr.msg_type == WHGP_MSG_RUMBLE && hdr.payload_size == sizeof(whgp_rumble_v1)) {
            whgp_rumble_v1 body{};
            if (!ReadExact(fd, &body, sizeof(body))) break;
            RumbleListener cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = rumbleListener_;
            }
            if (cb) cb(body.low, body.high, body.duration_ms);
            continue;
        }

        uint32_t left = hdr.payload_size;
        uint8_t discard[256];
        bool ok = true;
        while (left) {
            const uint32_t chunk = left > sizeof(discard) ? static_cast<uint32_t>(sizeof(discard)) : left;
            if (!ReadExact(fd, discard, chunk)) {
                ok = false;
                break;
            }
            left -= chunk;
        }
        if (!ok) break;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (clientFd_ == fd) {
            clientFd_ = -1;
            clientPid_ = -1;
        }
    }
    close(fd);
    LogThrottled("client recv loop exited", fd, -1);
}

void GamepadBridge::WriteState(int fd, uint32_t slot, const LogicalGamepadState& state)
{
    whgp_header hdr{};
    hdr.magic = WHGP_MAGIC;
    hdr.version = WHGP_VERSION;
    hdr.msg_type = WHGP_MSG_STATE;
    hdr.slot = slot;
    hdr.payload_size = sizeof(whgp_state_v1);

    whgp_state_v1 body{};
    body.buttons = state.buttons;
    body.lx = state.lx;
    body.ly = state.ly;
    body.rx = state.rx;
    body.ry = state.ry;
    body.lt = state.lt;
    body.rt = state.rt;
    body.hat_x = state.hatX;
    body.hat_y = state.hatY;

    const ssize_t total = static_cast<ssize_t>(sizeof(hdr) + sizeof(body));
    iovec iov[2] = {
        {&hdr, sizeof(hdr)},
        {&body, sizeof(body)},
    };
    std::lock_guard<std::mutex> lock(mutex_);
    if (clientFd_ != fd) return;
    if (writev(fd, iov, 2) != total) {
        shutdown(fd, SHUT_RDWR);
        if (clientFd_ == fd) clientFd_ = -1;
    }
}

void GamepadBridge::PublishState(uint32_t slot, const LogicalGamepadState& state)
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fd = clientFd_;
    }
    if (fd < 0) return;
    WriteState(fd, slot, state);
}

void GamepadBridge::AttachToHub()
{
    ControllerHub::Instance().SetEnabled(true);
    ControllerHub::Instance().SetStateListener(
        [this](uint32_t slot, const LogicalGamepadState& state) { PublishState(slot, state); });
}

}  // namespace controller
}  // namespace winehua
