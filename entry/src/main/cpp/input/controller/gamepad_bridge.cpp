#include "input/controller/gamepad_bridge.h"

#include "input/controller/controller_hub.h"
#include "input/controller/gamepad_ipc_protocol.h"

#include <cerrno>
#include <atomic>
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

struct GamepadBridge::Client {
    explicit Client(int socketFd) : fd(socketFd) {}
    const int fd;
    // Serialize whole packets with shutdown/close, including stale publisher
    // snapshots. A recycled descriptor must never receive another peer's state.
    std::mutex sendMutex;
    bool active = true;
    bool seen[kMaxControllerSlots] = {};
    uint64_t sequence[kMaxControllerSlots] = {};
    std::atomic<bool> finished{false};
    std::thread receiver;
};

GamepadBridge::~GamepadBridge()
{
    Stop();
}

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
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
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
    StopLocked();

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
    if (listen(fd, 8) != 0) {
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
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    StopLocked();
}

void GamepadBridge::StopLocked()
{
    int listenFd;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        listenFd = listenFd_;
        if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
    }
    // Do not close/reuse the listening descriptor until accept has returned.
    // Joining also finishes all client registration before taking the list.
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients.swap(clients_);
        if (listenFd >= 0) close(listenFd);
        listenFd_ = -1;
        if (!path_.empty()) unlink(path_.c_str());
    }
    for (const auto& client : clients) {
        std::lock_guard<std::mutex> sendLock(client->sendMutex);
        if (client->active) {
            client->active = false;
            shutdown(client->fd, SHUT_RDWR);
        }
    }
    for (const auto& client : clients) {
        if (client->receiver.joinable()) client->receiver.join();
    }
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

        std::vector<std::shared_ptr<Client>> finished;
        auto connection = std::make_shared<Client>(client);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                close(client);
                return;
            }
            for (auto it = clients_.begin(); it != clients_.end();) {
                if ((*it)->finished.load()) {
                    finished.push_back(*it);
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
            // Multiple winebus instances are legitimate. Replacing the old
            // socket makes them reconnect and evict each other indefinitely.
            clients_.push_back(connection);
        }
        for (const auto& ended : finished) ended->receiver.join();
        int peerPid = -1;
#ifdef SO_PEERCRED
        struct ucred credentials{};
        socklen_t length = sizeof(credentials);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0)
            peerPid = credentials.pid;
#endif
        OH_LOG_INFO(LOG_APP, "[WHGP] client connected fd=%{public}d peer_pid=%{public}d", client, peerPid);
        connection->receiver = std::thread([this, connection] { RecvLoop(connection); });
        // Joining a peer does not resend snapshots to existing consumers.
        for (uint32_t slot = 0; slot < kMaxControllerSlots; ++slot)
            WriteState(connection, slot, ControllerHub::Instance().GetState(slot));
    }
}

void GamepadBridge::RecvLoop(const std::shared_ptr<Client>& client)
{
    const int fd = client->fd;
    while (true) {
        whgp_header hdr{};
        if (!ReadExact(fd, &hdr, sizeof(hdr))) break;
        if (hdr.magic != WHGP_MAGIC) {
            OH_LOG_WARN(LOG_APP, "[WHGP] bad header from winebus magic=%{public}u ver=%{public}u",
                        hdr.magic, hdr.version);
            break;
        }
        if (!whgp_version_matches(hdr.version)) {
            OH_LOG_WARN(LOG_APP, "[WHGP] protocol mismatch: peer=%{public}u expected=%{public}u",
                        hdr.version, WHGP_VERSION);
            break;
        }
        if (hdr.payload_size > 4096 || hdr.slot >= kMaxControllerSlots) {
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
            if (cb) cb(hdr.slot, body.low, body.high, body.duration_ms);
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
        std::lock_guard<std::mutex> sendLock(client->sendMutex);
        client->active = false;
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    OH_LOG_INFO(LOG_APP, "[WHGP] client recv loop exited fd=%{public}d", fd);
    client->finished.store(true);
}

void GamepadBridge::WriteState(const std::shared_ptr<Client>& client, uint32_t slot,
                               const LogicalGamepadState& state)
{
    if (slot >= kMaxControllerSlots) return;
    whgp_header hdr{};
    hdr.magic = WHGP_MAGIC;
    hdr.version = WHGP_VERSION;
    hdr.msg_type = WHGP_MSG_STATE;
    hdr.slot = slot;
    hdr.payload_size = sizeof(whgp_state_v2);

    whgp_state_v2 body{};
    body.buttons = state.buttons;
    body.lx = state.lx;
    body.ly = state.ly;
    body.rx = state.rx;
    body.ry = state.ry;
    body.lt = state.lt;
    body.rt = state.rt;
    body.hat_x = state.hatX;
    body.hat_y = state.hatY;
    body.reserved[0] = WHGP_STATE_TOPOLOGY_VALID |
                       (state.connected ? WHGP_STATE_CONNECTED : 0);

    const ssize_t total = static_cast<ssize_t>(sizeof(hdr) + sizeof(body));
    iovec iov[2] = {
        {&hdr, sizeof(hdr)},
        {&body, sizeof(body)},
    };
    msghdr message{};
    message.msg_iov = iov;
    message.msg_iovlen = 2;
    std::lock_guard<std::mutex> sendLock(client->sendMutex);
    if (!client->active) return;
    // A concurrent hotplug/input update can overtake the initial snapshot.
    if (client->seen[slot] && state.sequence < client->sequence[slot]) return;
    client->seen[slot] = true;
    client->sequence[slot] = state.sequence;
    ssize_t sent;
    do {
        sent = sendmsg(client->fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
    if (sent != total) {
        // Never block input delivery behind a stalled peer or leave a partial
        // WHGP packet in the stream. winebus can reconnect for a fresh state.
        client->active = false;
        shutdown(client->fd, SHUT_RDWR);
    }
}

void GamepadBridge::PublishState(uint32_t slot, const LogicalGamepadState& state)
{
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients = clients_;
    }
    for (const auto& client : clients) WriteState(client, slot, state);
}

void GamepadBridge::AttachToHub()
{
    ControllerHub::Instance().SetEnabled(true);
    ControllerHub::Instance().SetStateListener(
        [this](uint32_t slot, const LogicalGamepadState& state) { PublishState(slot, state); });
}

}  // namespace controller
}  // namespace winehua
