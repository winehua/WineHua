#pragma once

// Queue-lifetime control only: no image data, no per-frame requests. Each
// NativeImage attachment gets a fresh socketpair, so a late reply from an old
// target can never retire a replacement queue. SOCK_SEQPACKET also preserves
// message boundaries across an OH IPC file-descriptor transfer.
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace winehua {
namespace queue_retirement {
constexpr uint32_t kMagic = 0x51525431;
enum Operation : uint32_t { Retire = 1, Acknowledge = 2, Resume = 3 };
struct Message { uint32_t magic = kMagic; uint32_t operation; uint64_t epoch; };
static_assert(sizeof(Message) == 16, "queue control wire layout");
inline bool Send(int fd, Operation op, uint64_t epoch)
{
    const Message msg{kMagic, op, epoch};
    ssize_t n;
    do { n = send(fd, &msg, sizeof(msg), MSG_DONTWAIT | MSG_NOSIGNAL); }
    while (n < 0 && errno == EINTR);
    return n == sizeof(msg);
}
inline uint64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace queue_retirement

enum class QueueRetirementResult { Ready, Pending, Failed };

class QueueRetirementProducer {
public:
    ~QueueRetirementProducer() { if (fd_ >= 0) close(fd_); }
    QueueRetirementProducer() = default;
    QueueRetirementProducer(const QueueRetirementProducer&) = delete;
    QueueRetirementProducer& operator=(const QueueRetirementProducer&) = delete;
    bool SetFd(int borrowedFd)
    {
        if (fd_ >= 0 || pending_) return false;
        if (borrowedFd < 0) return true; // Consumer without this capability.
        fd_ = fcntl(borrowedFd, F_DUPFD_CLOEXEC, 0);
        return fd_ >= 0;
    }
    QueueRetirementResult Begin()
    {
        using namespace queue_retirement;
        if (fd_ < 0 || closed_) return QueueRetirementResult::Ready;
        if (!pending_) {
            pending_ = ++epoch_;
            requestedMs_ = NowMs();
            if (!Send(fd_, Retire, pending_)) {
                if (errno == EPIPE || errno == ECONNRESET) {
                    closed_ = true;
                    return QueueRetirementResult::Ready;
                }
                return QueueRetirementResult::Failed;
            }
        }
        for (unsigned i = 0; i < 16; ++i) {
            Message msg{};
            const auto n = recv(fd_, &msg, sizeof(msg), MSG_DONTWAIT | MSG_TRUNC);
            if (n == 0) { closed_ = true; return QueueRetirementResult::Ready; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n != sizeof(msg) || msg.magic != kMagic)
                return QueueRetirementResult::Failed;
            if (msg.operation == Acknowledge && msg.epoch == pending_) {
                acknowledged_ = true;
                return QueueRetirementResult::Ready;
            }
        }
        if (acknowledged_) return QueueRetirementResult::Ready;
        return NowMs() - requestedMs_ >= 1000
            ? QueueRetirementResult::Failed : QueueRetirementResult::Pending;
    }
    // Device-owner teardown only. Caller must release manager AND target locks
    // before waiting: the compositor can be in Query or SetFramePeriod already.
    bool Wait(unsigned timeoutMs)
    {
        const uint64_t deadline = queue_retirement::NowMs() + timeoutMs;
        for (;;) {
            const auto result = Begin();
            if (result != QueueRetirementResult::Pending)
                return result == QueueRetirementResult::Ready;
            const auto now = queue_retirement::NowMs();
            if (now >= deadline) return false;
            pollfd pfd{fd_, POLLIN | POLLHUP, 0};
            const int resultCode = poll(&pfd, 1, static_cast<int>(deadline - now));
            if (resultCode < 0 && errno != EINTR) return false;
        }
    }
    bool Complete()
    {
        if (!pending_) return true;
        if (!closed_ && !queue_retirement::Send(fd_, queue_retirement::Resume, pending_))
            return false;
        pending_ = 0;
        acknowledged_ = false;
        return true;
    }
    bool Pending() const { return pending_ != 0; }
private:
    int fd_ = -1;
    uint64_t epoch_ = 0, pending_ = 0, requestedMs_ = 0;
    bool acknowledged_ = false, closed_ = false;
};

class QueueRetirementConsumer {
public:
    QueueRetirementConsumer() = default;
    ~QueueRetirementConsumer() { Close(); }
    QueueRetirementConsumer(const QueueRetirementConsumer&) = delete;
    QueueRetirementConsumer& operator=(const QueueRetirementConsumer&) = delete;
    bool Create()
    {
        if (fd_ >= 0) return false;
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair))
            return false;
        fd_ = pair[0]; offeredFd_ = pair[1];
        return true;
    }
    int OfferedFd() const { return offeredFd_; }
    void CloseOffer() { if (offeredFd_ >= 0) close(offeredFd_); offeredFd_ = -1; }
    void Close()
    {
        CloseOffer();
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
    // Called before any synchronous broker query on the compositor thread.
    // retire() must release retained scene references AND drain queued buffers.
    // A false result keeps the producer paused and retries next tick.
    template<typename RetireFn> bool Service(RetireFn retire)
    {
        using namespace queue_retirement;
        if (fd_ < 0) return false;
        for (unsigned i = 0; i < 16; ++i) {
            Message msg{};
            const auto n = recv(fd_, &msg, sizeof(msg), MSG_DONTWAIT | MSG_TRUNC);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n == 0) { closed_ = true; paused_ = true; break; }
            if (n != sizeof(msg) || msg.magic != kMagic) { paused_ = true; break; }
            if (msg.operation == Retire && msg.epoch > epoch_) {
                epoch_ = msg.epoch; paused_ = true; acknowledged_ = false;
            } else if (msg.operation == Resume && msg.epoch == epoch_ && acknowledged_) {
                paused_ = false;
            }
        }
        if (paused_ && !acknowledged_ && retire())
            acknowledged_ = closed_ || Send(fd_, Acknowledge, epoch_);
        return paused_;
    }
private:
    int fd_ = -1, offeredFd_ = -1;
    uint64_t epoch_ = 0;
    bool paused_ = false, acknowledged_ = false, closed_ = false;
};
} // namespace winehua
