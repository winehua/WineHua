#pragma once
#include "spawn_protocol.h"
#include <string>
// Prefix OPEN has already been consumed using recvmsg; never discard rights.
inline bool ReceiveOpenTarget(int fd, vp_received_fds& rights, int64_t deadline, std::string& target)
{
    char ch = 0;
    target.clear();
    if (rights.count || vp_recv_bytes(fd, &ch, 1, &rights, deadline) || ch != '\n' || rights.count) return false;
    while (target.size() <= 4090) {
        if (vp_recv_bytes(fd, &ch, 1, &rights, deadline) || rights.count) return false;
        if (ch == '\n') return !target.empty() && target.size() <= 4090;
        if (!ch || ch == '\r') return false;
        target.push_back(ch);
    }
    return false;
}
