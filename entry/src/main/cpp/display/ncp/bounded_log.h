#pragma once
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <initializer_list>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

// One fixed file, at most 4 MiB. A pipe keeps stdio and Xserver writes bounded
// without SIGXFSZ or per-frame filesystem checks on the compositor thread.
class DisplayChildLog {
public:
    bool Start(const char* path) {
        int fds[2];
        file_ = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (file_ < 0) return false;
        if (pipe2(fds, O_CLOEXEC) != 0) { close(file_); file_ = -1; return false; }
        savedOut_ = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        savedErr_ = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
        if (savedOut_ < 0 || savedErr_ < 0) {
            close(fds[0]); close(fds[1]); Close(); return false;
        }
        read_ = fds[0];
        try { reader_ = std::thread([this] { Drain(); }); }
        catch (...) { close(fds[1]); Close(); return false; }
        fflush(stdout); fflush(stderr);
        const bool ok = dup2(fds[1], STDOUT_FILENO) >= 0 && dup2(fds[1], STDERR_FILENO) >= 0;
        close(fds[1]);
        if (!ok) { Stop(); return false; }
        setvbuf(stdout, nullptr, _IOLBF, 0); setvbuf(stderr, nullptr, _IOLBF, 0);
        return true;
    }
    ~DisplayChildLog() { Stop(); }
    DisplayChildLog() = default;
    DisplayChildLog(const DisplayChildLog&) = delete;
    DisplayChildLog& operator=(const DisplayChildLog&) = delete;
private:
    void Drain() {
        constexpr size_t limit = 4 * 1024 * 1024;
        size_t stored = 0;
        char bytes[4096];
        auto stopping = std::chrono::steady_clock::time_point{};
        while (true) {
            if (stop_.load()) {
                const auto now = std::chrono::steady_clock::now();
                if (stopping == std::chrono::steady_clock::time_point{}) stopping = now;
                if (now - stopping >= std::chrono::milliseconds(250)) break;
            }
            pollfd fd{read_, POLLIN, 0};
            int rc = poll(&fd, 1, 100);
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0) break;
            if (!rc) { if (stop_.load()) break; continue; }
            ssize_t count = read(read_, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            if (stored + size_t(count) > limit) {
                if (ftruncate(file_, 0) != 0 || lseek(file_, 0, SEEK_SET) < 0) continue;
                stored = 0;
            }
            size_t offset = 0;
            while (offset < size_t(count)) {
                ssize_t written = write(file_, bytes + offset, size_t(count) - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) break;
                offset += size_t(written); stored += size_t(written);
            }
        }
    }
    void Stop() {
        if (savedOut_ >= 0) { fflush(stdout); dup2(savedOut_, STDOUT_FILENO); }
        if (savedErr_ >= 0) { fflush(stderr); dup2(savedErr_, STDERR_FILENO); }
        // Drain queued diagnostics, bounded even if another library duped a writer.
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
        Close();
    }
    void Close() {
        for (int* fd : {&read_, &file_, &savedOut_, &savedErr_}) {
            if (*fd >= 0) close(*fd);
            *fd = -1;
        }
    }
    int file_ = -1, read_ = -1, savedOut_ = -1, savedErr_ = -1;
    std::atomic<bool> stop_{false};
    std::thread reader_;
};
