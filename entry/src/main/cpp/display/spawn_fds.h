#pragma once
#include <AbilityKit/native_child_process.h>
#include <array>
#include <fcntl.h>
#include <unistd.h>

// wlroots owns its server FDs. The phone adapter consumes passed FDs after a
// handshake, while system NCP retains them. Give both backends private copies.
class DisplaySpawnFds {
public:
    explicit DisplaySpawnFds(const NativeChildProcess_Fd* source) {
        for (; source; source = source->next) {
            if (count_ == nodes_.size()) { valid_ = false; break; }
            auto& node = nodes_[count_];
            node.fd = fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
            if (node.fd < 0) { valid_ = false; break; }
            node.fdName = source->fdName;
            if (count_) nodes_[count_ - 1].next = &node;
            ++count_;
        }
    }
    DisplaySpawnFds(const DisplaySpawnFds&) = delete;
    DisplaySpawnFds& operator=(const DisplaySpawnFds&) = delete;
    ~DisplaySpawnFds() { if (!consumed_) for (size_t i=0;i<count_;++i) close(nodes_[i].fd); }
    bool Valid() const { return valid_; }
    NativeChildProcess_Fd* Head() { return count_ ? nodes_.data() : nullptr; }
    void Consumed() { consumed_ = true; }
private:
    std::array<NativeChildProcess_Fd, 16> nodes_{};
    size_t count_ = 0;
    bool valid_ = true, consumed_ = false;
};
