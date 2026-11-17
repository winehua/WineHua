#include "proc/open_protocol.h"
#include "proc/ncp_dispatch.h"
/* Broker v2: bounded readers feed one serialized NCP dispatcher. */
#include "broker.h"
#include "bridge/system_open.h"
#include "wine/wine_env.h"
#include "spawn_codec.h"
#include "wine_image_policy.h"
#include "wine_spawn_compatibility.h"
#include "common/wait_utils.h"
#include "wine/wine_constants.h"
#include "audio/audio_broker.h"
#include "wine_process.h"
#include "wine_child_ipc_launcher.h"
#include "phone_adapter/phone_adapter.h"
#include "phone_adapter/phone_process.h"
#include "cef_utility_probe.h"
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "WL_Broker"

std::string gBrokerHomeDir;
std::string gBrokerPrefixDir;
static const char* kBrokerSocketPath = WINE_BROKER_SOCKET;
static std::atomic<bool> gBrokerRunning{false};
static std::atomic<bool> gDirectNcpSessionDefault{false};

void SetBrokerDirectNcpSessionDefault(bool enabled) {
    if (PhoneAdapter_IsPhoneMode()) enabled = false;
    gDirectNcpSessionDefault.store(enabled, std::memory_order_release);
    OH_LOG_WARN(LOG_APP, "[Broker] Create NCP session default=%{public}d", enabled ? 1 : 0);
}

static int32_t QueryPeerPid(int fd) {
    struct { pid_t pid; uid_t uid; gid_t gid; } cred{};
    socklen_t size = sizeof(cred);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0 ? cred.pid : -1;
}
static void CloseOwnedFdIfRetained(int fd, const struct stat& original, bool haveOriginal) {
    struct stat current{};
    if (haveOriginal && fstat(fd, &current) == 0 &&
        current.st_dev == original.st_dev && current.st_ino == original.st_ino) close(fd);
}
static void Reply(int fd, int32_t pid, int32_t status, int timeoutMs = 1000) {
    unsigned char bytes[8]; vp_store32(bytes, static_cast<uint32_t>(pid));
    vp_store32(bytes + 4, static_cast<uint32_t>(status));
    if (vp_send_bytes(fd, bytes, sizeof(bytes), nullptr, 0, vp_now_ms() + timeoutMs))
        OH_LOG_WARN(LOG_APP, "[Broker] response failed errno=%{public}d", errno);
}
struct BrokerJob {
    explicit BrokerJob(int fd) : conn(fd), acceptedMs(vp_now_ms()), peerPid(QueryPeerPid(fd)) {}
    ~BrokerJob() { vp_close_fds(&fds); close(conn); }
    int conn;
    int64_t acceptedMs;
    int32_t peerPid;
    vp_received_fds fds{};
    winehua::spawn::Request request;
};
struct BrokerQueues {
    std::mutex mutex;
    std::condition_variable readable, launchable;
    bool running = true;
    std::deque<std::unique_ptr<BrokerJob>> incoming, ready;
};
static constexpr size_t kQueueCapacity = 16;
static constexpr size_t kReaderCount = 4;

static bool ReceiveJob(BrokerJob& job) {
    unsigned char prefix[4];
    if (vp_recv_bytes(job.conn, prefix, 4, &job.fds, job.acceptedMs + 1000)) return false;
    if (!memcmp(prefix, "OPEN", 4)) {
        std::string target;
        bool accepted = ReceiveOpenTarget(job.conn, job.fds, job.acceptedMs + 1000, target) &&
            DispatchSystemOpen(target);
        Reply(job.conn, accepted ? 0 : -1, 0);
        return false;
    }
    vp_spawn_spec spec{};
    const int result = vp_recv_request_prefix(job.conn, &spec, &job.fds, job.acceptedMs + 3000, prefix);
    const int error = errno;
    if (!result) winehua::spawn::Copy(spec, job.request);
    vp_free_spec(&spec);
    if (result) {
        // Readiness probes close without submitting a request.
        if (error != ECONNRESET) Reply(job.conn, -1, -error);
        return false;
    }
    for (const auto& name : job.request.names) {
        if (name == "wine_audio_bootstrap") {
            Reply(job.conn, -1, NCP_ERR_INVALID_PARAM);
            return false;
        }
    }
    OH_LOG_INFO(LOG_APP, "[Broker] request peerPid=%{public}d argc=%{public}zu envCount=%{public}zu fdCount=%{public}zu",
                job.peerPid, job.request.argv.size(), job.request.env.size(), job.fds.count);
    return true;
}

static void LaunchJob(BrokerJob& job) {
    auto& request = job.request;
    if (vp_now_ms() - job.acceptedMs > 20000 || gBrokerHomeDir.empty()) {
        Reply(job.conn, -1, gBrokerHomeDir.empty() ? NCP_ERR_INVALID_PARAM : NCP_ERR_TIMEOUT);
        return;
    }
    request.home = gBrokerHomeDir;
    AppendWineDpiEnv(request.env);
    if (!gBrokerPrefixDir.empty()) request.env.emplace_back("WINEPREFIX=" + gBrokerPrefixDir);
    const bool freshImage = winehua::spawn::RequiresFreshWineProcess(request);
    const auto freshGraphics = winehua::spawn::ConfigureFreshWineGraphics(request, freshImage, request.bin);
    if (freshGraphics == winehua::spawn::FreshWineGraphics::MissingRuntime) {
        OH_LOG_ERROR(LOG_APP, "[PROC-IMAGE] clean process requires packaged Venus runtime");
        Reply(job.conn, -1, -ENOENT); return;
    }
    if (freshGraphics == winehua::spawn::FreshWineGraphics::Venus)
        OH_LOG_INFO(LOG_APP, "[PROC-IMAGE] clean PE32 process uses Venus with the active desktop preciseMaps=%{public}s mappedFlush=%{public}s ringPerf=%{public}s",
            winehua::spawn::ImageEnvironment(request, "WINEHUA_VK_PRECISE_MAP").c_str(),
            winehua::spawn::ImageEnvironment(request, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED").c_str(),
            winehua::spawn::ImageEnvironment(request, "VN_WINEHUA_PERF_SUMMARY").c_str());
    // Verify the final parameter budget before creating audio resources or a child.
    std::string entry;
    if (!winehua::spawn::EncodeEntry(request, entry)) {
        Reply(job.conn, -1, -E2BIG); return;
    }
    int audioFd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (audioFd >= 0) request.names.emplace_back("wine_audio_bootstrap");
    if (!winehua::spawn::EncodeEntry(request, entry)) {
        if (audioFd >= 0) close(audioFd);
        Reply(job.conn, -1, -E2BIG); return;
    }
    NativeChildProcess_Fd nodes[VP_FDS_CAP]{};
    struct stat identities[VP_FDS_CAP]{};
    bool haveIdentity[VP_FDS_CAP]{};
    for (size_t i = 0; i < request.names.size(); ++i) {
        nodes[i].fdName = request.names[i].data();
        nodes[i].fd = i < job.fds.count ? job.fds.items[i] : audioFd;
        nodes[i].next = i + 1 < request.names.size() ? &nodes[i + 1] : nullptr;
        haveIdentity[i] = fstat(nodes[i].fd, &identities[i]) == 0;
    }
    NativeChildProcess_Args args{};
    args.entryParams = entry.data(); args.fdList.head = request.names.empty() ? nullptr : nodes;
    NativeChildProcess_Options options{}; options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    const bool direct = !freshImage && winehua::spawn::EnvFlag(request, "WINEHUA_DIRECT_NCP",
        gDirectNcpSessionDefault.load(std::memory_order_acquire));
    if (freshImage)
        OH_LOG_INFO(LOG_APP, "[PROC-IMAGE] preferred PE32 range occupied; fresh native process exe=%{public}s",
                    winehua::spawn::ProcessPath(request).c_str());
    const bool phoneFork = !direct && PhoneAdapter_IsPhoneMode() &&
        winehua::spawn::EnvFlag(request, "WINEHUA_PHONE_DIRECT_FORK");
    int32_t childPid = -1;
    const uint64_t spawnTicket = BeginNativeProcessSpawn();
    const int32_t status = direct ? (PhoneAdapter_IsPhoneMode() ? NCP_ERR_NOT_SUPPORTED :
        StartWineChildViaIpc(args, &childPid)) : phoneFork ?
        Phone_StartViaDirectForkServer("libwine_child.so:Main", args, &childPid) :
        WineHua_StartNativeChildProcess(const_cast<char*>("libwine_child.so:Main"), args, options, &childPid);
    OH_LOG_INFO(LOG_APP, "[PROC-SPAWN] brokerHostPid=%{public}d creatorHostPid=%{public}d childHostPid=%{public}d createStatus=%{public}d mode=%{public}s",
                getpid(), job.peerPid, childPid, status, direct ? "create-ipc" : phoneFork ? "phone-fork-server" :
                PhoneAdapter_IsPhoneMode() ? "phone-fork" : "system-start");
    if (!status && childPid > 0) {
        AddProcess(childPid, winehua::spawn::ProcessPath(request), -1,
            !direct && !phoneFork && PhoneAdapter_IsPhoneMode()
                ? ProcessRegistration::Relabel : ProcessRegistration::NativeChild, spawnTicket);
        if (phoneFork) Phone_MarkDirectForkChildRegistered(childPid);
        WineHuaCefUtilityProbeNoteSpawn(childPid, job.peerPid, entry.c_str());
        if (direct) MarkWineIpcChildRegistered(childPid);
    }
    for (size_t i = 0; i < request.names.size(); ++i)
        CloseOwnedFdIfRetained(nodes[i].fd, identities[i], haveIdentity[i]);
    job.fds.count = 0; // NCP may close originals; the identity check above owns cleanup.
    Reply(job.conn, childPid, status);
}

static void Reader(const std::shared_ptr<BrokerQueues>& queues) {
    for (;;) {
        std::unique_ptr<BrokerJob> job;
        {
            std::unique_lock<std::mutex> lock(queues->mutex);
            queues->readable.wait(lock, [&] { return !queues->running || !queues->incoming.empty(); });
            if (!queues->running) return;
            job = std::move(queues->incoming.front()); queues->incoming.pop_front();
        }
        if (!ReceiveJob(*job)) continue;
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(queues->mutex);
            if (queues->running && queues->ready.size() < kQueueCapacity) {
                queues->ready.push_back(std::move(job)); queued = true;
            }
        }
        if (queued) queues->launchable.notify_one();
        else Reply(job->conn, -1, NCP_ERR_BUSY);
    }
}
static void Dispatcher(const std::shared_ptr<BrokerQueues>& queues) {
    for (;;) {
        std::unique_ptr<BrokerJob> job;
        {
            std::unique_lock<std::mutex> lock(queues->mutex);
            queues->launchable.wait(lock, [&] { return !queues->running || !queues->ready.empty(); });
            if (!queues->running) return;
            job = std::move(queues->ready.front()); queues->ready.pop_front();
        }
        // Create callback state is process-global; serialize all NCP API calls.
        LaunchJob(*job);
    }
}
static void BrokerThreadFunc() {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (strlen(kBrokerSocketPath) >= sizeof(address.sun_path)) {
        OH_LOG_ERROR(LOG_APP, "[Broker] socket path exceeds Unix socket limit");
        if (fd >= 0) close(fd);
        gBrokerRunning.store(false); return;
    }
    strcpy(address.sun_path, kBrokerSocketPath);
    unlink(kBrokerSocketPath);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(fd, 32)) {
        OH_LOG_ERROR(LOG_APP, "[Broker] socket setup failed errno=%{public}d", errno);
        if (fd >= 0) close(fd);
        gBrokerRunning.store(false); return;
    }
    auto queues = std::make_shared<BrokerQueues>();
    for (size_t i = 0; i < kReaderCount; ++i) std::thread(Reader, queues).detach();
    std::thread(Dispatcher, queues).detach();
    while (gBrokerRunning.load(std::memory_order_acquire)) {
        int conn = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR) continue;
            OH_LOG_ERROR(LOG_APP, "[Broker] accept failed errno=%{public}d", errno);
            break;
        }
        auto job = std::make_unique<BrokerJob>(conn);
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(queues->mutex);
            if (queues->incoming.size() < kQueueCapacity) {
                queues->incoming.push_back(std::move(job)); queued = true;
            }
        }
        if (queued) queues->readable.notify_one();
        else Reply(job->conn, -1, NCP_ERR_BUSY, 10);
    }
    {
        std::lock_guard<std::mutex> lock(queues->mutex);
        queues->running = false; queues->incoming.clear(); queues->ready.clear();
    }
    queues->readable.notify_all(); queues->launchable.notify_all();
    close(fd); unlink(kBrokerSocketPath); gBrokerRunning.store(false);
}

int StartBrokerServer()
{
    bool expected = false;
    if (!gBrokerRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        OH_LOG_WARN(LOG_APP, "[Broker] already running");
        return 0;
    }

    std::thread(BrokerThreadFunc).detach();

    // 就绪判定必须真实 connect: bind() 一成功 socket 文件就存在, 但 listen()
    // 尚未完成时 connect 会拿 ECONNREFUSED — 曾致紧随其后的 wineserver
    // broker spawn 失败 (state:failed:wineserver → "启动失败")。探测连接在
    // HandleRequest 的 recvmsg 处拿 EOF 被忽略, 无副作用。
    if (!WaitFor("broker socket", []() {
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return false;
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strcpy(addr.sun_path, kBrokerSocketPath);
            const bool ok = connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
            close(fd);
            return ok;
        }, 2000, 50)) {
        OH_LOG_WARN(LOG_APP, "[Broker] socket creation slow, continuing anyway");
        if (!gBrokerRunning.load(std::memory_order_acquire)) return -1;
    }
    return 0;
}
