#include "direct_native_read_watchdog.h"
#include <hidebug/hidebug.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <cerrno>

namespace winehua::direct::shared {
struct NativeReadWatchdog::Impl {
    static Impl* active;
    void* library = nullptr;
    HiDebug_Backtrace_Object object = nullptr;
    decltype(&OH_HiDebug_BacktraceFromFp) unwind = nullptr;
    decltype(&OH_HiDebug_SymbolicAddress) symbolize = nullptr;
    decltype(&OH_HiDebug_DestroyBacktraceObject) destroy = nullptr;
    pthread_t mainThread = pthread_self();
    struct sigaction previous{};
    sigset_t previousMask{};
    bool installed = false;
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false, stopping = false;
    std::thread worker;
    void* pcs[16]{};
    int count = 0;
    std::atomic<bool> captured{false};

    static void Capture(int, siginfo_t*, void* context) {
        const int saved = errno;
        if (active && context) {
            auto* state = static_cast<ucontext_t*>(context);
            active->pcs[0] = reinterpret_cast<void*>(state->uc_mcontext.pc);
            active->count = 1 + active->unwind(active->object,
                reinterpret_cast<void*>(state->uc_mcontext.regs[29]), active->pcs + 1, 15);
            active->captured.store(true, std::memory_order_release);
        }
        errno = saved;
    }
    static void Symbol(void*, void* argument, const HiDebug_StackFrame* frame) {
        if (!frame || frame->type != HIDEBUG_STACK_FRAME_TYPE_NATIVE) return;
        const auto& native = frame->frame.native;
        fprintf(stderr, "[P0STACK] #%d pc=%llx %.120s %.96s+%llu\n", *static_cast<int*>(argument),
            static_cast<unsigned long long>(native.relativePc), native.mapName ? native.mapName : "?",
            native.functionName ? native.functionName : "?", static_cast<unsigned long long>(native.funcOffset));
    }
    Impl() {
        static_assert(std::atomic<bool>::is_always_lock_free, "signal capture flag");
        library = dlopen("libohhidebug.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) { fprintf(stderr, "[P0STACK] HiDebug unavailable\n"); return; }
        auto create = reinterpret_cast<decltype(&OH_HiDebug_CreateBacktraceObject)>(
            dlsym(library, "OH_HiDebug_CreateBacktraceObject"));
        unwind = reinterpret_cast<decltype(unwind)>(dlsym(library, "OH_HiDebug_BacktraceFromFp"));
        symbolize = reinterpret_cast<decltype(symbolize)>(dlsym(library, "OH_HiDebug_SymbolicAddress"));
        destroy = reinterpret_cast<decltype(destroy)>(dlsym(library, "OH_HiDebug_DestroyBacktraceObject"));
        if (!create || !unwind || !symbolize || !destroy || !(object = create())) {
            fprintf(stderr, "[P0STACK] HiDebug API unavailable\n"); return;
        }
        struct sigaction action{};
        action.sa_sigaction = Capture;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        active = this;
        if (sigaction(SIGUSR2, &action, &previous) != 0) { active = nullptr; return; }
        sigset_t captureMask;
        sigemptyset(&captureMask);
        sigaddset(&captureMask, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &captureMask, &previousMask);
        installed = true;
        worker = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            for (;;) {
                changed.wait(lock, [this] { return armed || stopping; });
                if (stopping) return;
                if (!changed.wait_for(lock, std::chrono::seconds(3), [this] { return !armed || stopping; })) break;
                if (stopping) return;
            }
            lock.unlock();
            if (pthread_kill(mainThread, SIGUSR2) != 0) return;
            for (int i = 0; i < 200 && !captured.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!captured.load(std::memory_order_acquire)) { fprintf(stderr, "[P0STACK] no capture\n"); return; }
            fprintf(stderr, "[P0STACK] Native API stalled; frames=%d\n", count);
            for (int i = 0; i < count && i < 16; ++i) symbolize(object, pcs[i], &i, Symbol);
            fflush(stderr);
        });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; changed.notify_all(); }
        if (worker.joinable()) worker.join();
        if (installed) {
            sigaction(SIGUSR2, &previous, nullptr);
            pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
            active = nullptr;
        }
        if (object && destroy) destroy(object);
        if (library) dlclose(library);
    }
};
NativeReadWatchdog::Impl* NativeReadWatchdog::Impl::active = nullptr;
NativeReadWatchdog::NativeReadWatchdog() : impl_(new Impl()) {}
NativeReadWatchdog::~NativeReadWatchdog() = default;
void NativeReadWatchdog::Arm() {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->armed = true; impl_->changed.notify_all();
}
void NativeReadWatchdog::Disarm() {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->armed = false; impl_->changed.notify_all();
}
}
