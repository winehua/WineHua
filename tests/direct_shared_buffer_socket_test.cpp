// Linux host check of the actual SCM_RIGHTS transport: fd aliasing and failed
// receive cleanup. Harmony NativeBuffer/GPU correctness is checked on-device.
#include "direct/native_buffer_socket.h"
#include <cassert>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>

using namespace winehua::direct::shared;
int Fds() {
    auto* d = opendir("/proc/self/fd");
    assert(d);
    int count = -1;
    while (auto* entry = readdir(d)) if (entry->d_name[0] != '.') ++count;
    closedir(d);
    return count;
}
void Raw(int socket, Packet p, int fd, size_t fdCount = 1, size_t bytes = sizeof(Packet)) {
    iovec data{&p, bytes};
    alignas(cmsghdr) char control[CMSG_SPACE(32 * sizeof(int))]{};
    msghdr m{};
    m.msg_iov = &data; m.msg_iovlen = 1;
    if (fdCount) {
        m.msg_control = control; m.msg_controllen = CMSG_SPACE(fdCount * sizeof(int));
        auto* h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(fdCount * sizeof(int));
        for (size_t i = 0; i < fdCount; ++i) reinterpret_cast<int*>(CMSG_DATA(h))[i] = fd;
    }
    assert(sendmsg(socket, &m, MSG_NOSIGNAL) == static_cast<ssize_t>(bytes));
}
int main() {
    int channel[2], backing[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) == 0);
    assert(pipe(backing) == 0);
    const int baseline = Fds();
    {
        Packet p{}; p.kind = Kind::Buffer;
        assert(Send(channel[0], p, {backing[0]}));
        Message m;
        assert(Receive(channel[1], m, 1000) && m.fds.size() == 1);
        assert(m.fds[0] != backing[0] && (fcntl(m.fds[0], F_GETFD) & FD_CLOEXEC));
        const char expected[] = "same kernel backing resource";
        assert(write(backing[1], expected, sizeof(expected)) == sizeof(expected));
        char observed[sizeof(expected)]{};
        assert(read(m.fds[0], observed, sizeof(observed)) == sizeof(observed));
        assert(!memcmp(expected, observed, sizeof(expected)));
    }
    assert(Fds() == baseline);
    for (int i = 0; i < 100; ++i) {
        Packet p{}; p.fdCount = 1;
        Message m;
        p.magic = 0;
        Raw(channel[0], p, backing[0]);
        assert(!Receive(channel[1], m, 1000) && m.fds.empty());
        p.magic = kMagic; p.fdCount = 2;
        Raw(channel[0], p, backing[0]); // metadata/ancillary count disagreement
        assert(!Receive(channel[1], m, 1000) && m.fds.empty());
        p.fdCount = 17;
        Raw(channel[0], p, backing[0], 17); // truncated ancillary data
        assert(!Receive(channel[1], m, 1000) && m.fds.empty());
        p.fdCount = 1;
        Raw(channel[0], p, backing[0], 1, sizeof(Packet) - 1);
        assert(!Receive(channel[1], m, 1000) && m.fds.empty());
        assert(Fds() == baseline);
    }
    Message absent;
    assert(!Receive(channel[1], absent, 1));
    assert(!Send(channel[0], Packet{}, {-1}));
    close(channel[0]); close(channel[1]); close(backing[0]); close(backing[1]);
    puts("shared-buffer socket PASS: aliasing, CLOEXEC, 400 invalid packets, no fd leak");
}
