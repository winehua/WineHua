#include "native_buffer_socket.h"
#include <cerrno>
#include <cstring>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace winehua::direct::shared {
Message::~Message() { CloseFds(); }
void Message::CloseFds() { for (int fd : fds) if (fd >= 0) close(fd); fds.clear(); }

bool Send(int socket, Packet packet, const std::vector<int>& fds) {
    if (fds.size() > kMaxFds) return false;
    for (int fd : fds) if (fd < 0) return false;
    packet.fdCount = static_cast<uint32_t>(fds.size());
    iovec data{&packet, sizeof(packet)};
    alignas(cmsghdr) char control[CMSG_SPACE(kMaxFds * sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    if (!fds.empty()) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
        memcpy(CMSG_DATA(header), fds.data(), fds.size() * sizeof(int));
    }
    ssize_t count;
    do { count = sendmsg(socket, &message, MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
    return count == sizeof(packet);
}

bool Receive(int socket, Message& output, int timeoutMs) {
    output.CloseFds();
    pollfd input{socket, POLLIN, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int ready;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        ready = poll(&input, 1, static_cast<int>(remaining > 0 ? remaining : 0));
        if (ready >= 0 || errno != EINTR || std::chrono::steady_clock::now() >= deadline) break;
    }
    if (ready <= 0 || !(input.revents & POLLIN)) return false;
    iovec data{&output.packet, sizeof(output.packet)};
    alignas(cmsghdr) char control[CMSG_SPACE(kMaxFds * sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t count;
    do { count = recvmsg(socket, &message, MSG_CMSG_CLOEXEC); } while (count < 0 && errno == EINTR);
    for (auto* h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS || h->cmsg_len < CMSG_LEN(0)) continue;
        const size_t n = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(h));
        output.fds.insert(output.fds.end(), fds, fds + n);
    }
    const auto& p = output.packet;
    if (count != sizeof(Packet) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        p.magic != kMagic || p.version != kVersion || p.size != sizeof(Packet) ||
        p.fdCount != output.fds.size() || p.fdCount > kMaxFds ||
        !memchr(p.stage, 0, sizeof(p.stage)) || !memchr(p.loaderPath, 0, sizeof(p.loaderPath)) ||
        !memchr(p.deviceName, 0, sizeof(p.deviceName))) {
        output.CloseFds();
        return false;
    }
    return true;
}

} // namespace winehua::direct::shared
