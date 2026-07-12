/**
 * broker.cpp — Process Broker: Unix socket server
 *
 * 在主进程中运行，接收来自 spawn_process (ntdll.so) 的子进程创建请求。
 * 每个请求包含 entryParams 字符串 + N 个命名 fd (SCM_RIGHTS, 可选 FDS 命名行)。
 * Broker 在主进程上下文调用 OH_Ability_StartNativeChildProcess，
 * 从而绕过 appspawn 子进程中无法嵌套调用 NCP API 的限制。
 *
 * 协议 (简单二进制):
 *   请求: "SPAWN\n{entryParams}\n[FDS:name0,name1,...\n]" + SCM_RIGHTS{N fd, N<=16}
 *   响应: [childPid: int32_le] [status: int32_le]   (8 字节)
 */
#include "broker.h"
#include "wait_utils.h"
#include "wine_constants.h"
#include "wine_env.h"
#include "audio_broker.h"
// 由 LaunchPadMode 在启动 Broker 前设置
std::string gBrokerHomeDir;

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>
#include <AbilityKit/native_child_process.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "WL_Broker"
#include <hilog/log.h>

static const char* kBrokerSocketPath = WINE_BROKER_SOCKET;

static std::atomic<bool> gBrokerRunning{false};
static std::mutex gBrokerSessionEnvMutex;
static std::vector<std::string> gBrokerSessionEnv;

// 处理单个请求: recvmsg(entryParams + fd) → StartNativeChildProcess → sendmsg(childPid, status)
static void HandleRequest(int conn_fd)
{
    OH_LOG_INFO(LOG_APP, "[Broker] handling request on fd=%{public}d", conn_fd);

    // 1) 接收请求头 + entryParams
    char buf[16384];
    memset(buf, 0, sizeof(buf));

    struct msghdr msg = {};
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf) - 1;

    // SCM_RIGHTS 控制消息缓冲区 (最多接收 kMaxFds 个 fd)
    static const int kMaxFds = 16;  // OHOS NativeChildProcess_FdList 上限
    union {
        char buf[CMSG_SPACE(sizeof(int) * 16)];
        struct cmsghdr align;
    } ctrl;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    ssize_t n = recvmsg(conn_fd, &msg, 0);
    if (n <= 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] recvmsg failed: %{public}s", strerror(errno));
        close(conn_fd);
        return;
    }
    buf[n] = '\0';

    // 2) 解析 "SPAWN\n{entryParams}\n[FDS:name0,name1,...\n][ENV:{n}\n{blob}]"
    //    entryParams 到第一个 '\n' 为止; 其后是可选段: FDS: (逗号分隔 fd 名) 和/或
    //    ENV:{n}\n (n 字节环境变量 blob, \0 分隔的 KEY=VALUE)。
    if (strncmp(buf, "SPAWN\n", 6) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] bad protocol: %{public}s", buf);
        close(conn_fd);
        return;
    }
    char* entryParamsRaw = buf + 6;
    char* fdsLine = nullptr;
    char* envBlob = nullptr;
    int envBlobLen = 0;
    {
        char* nl = strchr(entryParamsRaw, '\n');
        if (nl) {
            *nl = '\0';  // 截断 entryParams
            char* rest = nl + 1;
            if (strncmp(rest, "FDS:", 4) == 0) {
                fdsLine = rest + 4;
                char* nl2 = strchr(fdsLine, '\n');
                if (nl2) {
                    *nl2 = '\0';
                    rest = nl2 + 1;  // rest 指向 FDS 行之后的 "\0"
                }
            }
            // ENV:{n}\n{blob} — blob 含 \0, 长度由 {n} 精确指定
            // 跳过头部的 \n (FDS 行尾 \n + ENV 头 \n 可能连续出现)
            while (*rest == '\n') rest++;
            if (strncmp(rest, "ENV:", 4) == 0) {
                char* envN = rest + 4;
                char* envNl = strchr(envN, '\n');
                if (envNl) {
                    *envNl = '\0';
                    envBlobLen = atoi(envN);
                    if (envBlobLen > 0 && envBlobLen <= 65536)  // 64KB 安全上限
                        envBlob = envNl + 1;  // blob 紧贴 \n 之后
                    else
                        envBlobLen = 0;
                }
            }
        }
    }

    OH_LOG_INFO(LOG_APP, "[Broker] request entryParams=%{public}s fds=%{public}s env=%{public}d",
                entryParamsRaw, fdsLine ? fdsLine : "(none)", envBlobLen);

    // 3) 提取 fd (SCM_RIGHTS, 可能多个)
    int recvFds[kMaxFds];
    int nFds = 0;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int cnt = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (cnt < 0) cnt = 0;
        if (cnt > kMaxFds) {
            OH_LOG_WARN(LOG_APP, "[Broker] received %{public}d fds > max %{public}d, truncating", cnt, kMaxFds);
            cnt = kMaxFds;
        }
        memcpy(recvFds, CMSG_DATA(cmsg), cnt * sizeof(int));
        nFds = cnt;
        OH_LOG_INFO(LOG_APP, "[Broker] received %{public}d fd(s) via SCM_RIGHTS", nFds);
    }

    // 4) 解析 fd 名字列表 (逗号分隔)
    char* fdNames[kMaxFds] = {};
    int nNames = 0;
    if (fdsLine) {
        char* saveptr = nullptr;
        for (char* tok = strtok_r(fdsLine, ",", &saveptr); tok && nNames < kMaxFds;
             tok = strtok_r(nullptr, ",", &saveptr)) {
            fdNames[nNames++] = tok;
        }
        if (nNames != nFds) {
            OH_LOG_WARN(LOG_APP, "[Broker] FDS name count %{public}d != fd count %{public}d", nNames, nFds);
        }
    }

    // 5) 构造 NativeChildProcess 参数
    // 复制 entryParams 并加上 homeDir 前缀 (与 LaunchPadMode 新格式一致)
    std::string fullParams = gBrokerHomeDir.empty() ? entryParamsRaw
                            : (gBrokerHomeDir + "|" + entryParamsRaw);
    std::vector<std::string> sessionEnv;
    {
        std::lock_guard<std::mutex> lock(gBrokerSessionEnvMutex);
        sessionEnv = gBrokerSessionEnv;
    }
    if (!sessionEnv.empty()) {
        size_t appended = AppendMissingEntryParamsEnvOverrides(fullParams, sessionEnv);
        if (appended > 0) {
            OH_LOG_INFO(LOG_APP,
                        "[Broker] appended missing session env overrides count=%{public}zu/%{public}zu",
                        appended, sessionEnv.size());
        }
    }
    char* entryParamsCopy = strdup(fullParams.c_str());

    // 建 fd 链表: 名字取自 FDS 行; 无 FDS 行且恰好 1 个 fd 时回退旧命名 wineserver_sock
    NativeChildProcess_Fd nodes[kMaxFds];
    memset(nodes, 0, sizeof(nodes));
    int nNodes = 0;
    for (int i = 0; i < nFds; i++) {
        const char* name = nullptr;
        if (fdsLine && i < nNames) {
            name = fdNames[i];
        } else if (!fdsLine && nFds == 1) {
            name = "wineserver_sock";  // 向后兼容旧协议
        } else {
            OH_LOG_WARN(LOG_APP, "[Broker] fd[%{public}d]=%{public}d has no name, skipping", i, recvFds[i]);
            continue;
        }
        if (strlen(name) > 20) {
            OH_LOG_WARN(LOG_APP, "[Broker] fdName '%{public}s' exceeds 20 chars (OHOS limit)", name);
        }
        nodes[nNodes].fdName = const_cast<char*>(name);
        nodes[nNodes].fd = recvFds[i];
        nodes[nNodes].next = nullptr;
        if (nNodes > 0) nodes[nNodes - 1].next = &nodes[nNodes];
        OH_LOG_INFO(LOG_APP, "[Broker] fd[%{public}d] name=%{public}s fd=%{public}d", nNodes, name, recvFds[i]);
        nNodes++;
    }

    // 若有 ENV blob, 写入 memfd 并以命名 fd "wine_env" 传给子进程
    int envMemFd = -1;
    if (envBlob && envBlobLen > 0 && nNodes < kMaxFds) {
#ifdef __linux__
        envMemFd = memfd_create("wine_env", MFD_CLOEXEC);
#endif
        if (envMemFd < 0) {
            // fallback: shm_open (musl / OHOS 可能缺 memfd_create)
            envMemFd = shm_open("/wine_env_brk", O_RDWR | O_CREAT | O_EXCL, 0600);
            if (envMemFd >= 0) {
                shm_unlink("/wine_env_brk");
                if (ftruncate(envMemFd, envBlobLen) < 0) {
                    close(envMemFd);
                    envMemFd = -1;
                }
            }
        }
        if (envMemFd >= 0) {
            ssize_t written = write(envMemFd, envBlob, envBlobLen);
            if (written == envBlobLen) {
                lseek(envMemFd, 0, SEEK_SET);
                nodes[nNodes].fdName = const_cast<char*>("wine_env");
                nodes[nNodes].fd = envMemFd;
                nodes[nNodes].next = nullptr;
                if (nNodes > 0) nodes[nNodes - 1].next = &nodes[nNodes];
                OH_LOG_INFO(LOG_APP, "[Broker] env memfd=%{public}d len=%{public}d (fd node %{public}d)",
                            envMemFd, envBlobLen, nNodes);
                nNodes++;
            } else {
                OH_LOG_WARN(LOG_APP, "[Broker] env memfd write failed: %{public}zd != %{public}d",
                            written, envBlobLen);
                close(envMemFd);
                envMemFd = -1;
            }
        } else {
            OH_LOG_WARN(LOG_APP, "[Broker] env memfd create failed (errno=%{public}d), skipping env",
                        errno);
        }
    }

    int audioBootstrapFd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (audioBootstrapFd >= 0 && nNodes < kMaxFds) {
        nodes[nNodes].fdName = const_cast<char*>("wine_audio_bootstrap");
        nodes[nNodes].fd = audioBootstrapFd;
        if (nNodes > 0) nodes[nNodes - 1].next = &nodes[nNodes];
        nNodes++;
    }

    NativeChildProcess_FdList fdList = {};
    fdList.head = (nNodes > 0) ? &nodes[0] : nullptr;
    NativeChildProcess_Args args = {};
    args.entryParams = entryParamsCopy;
    args.fdList = fdList;

    NativeChildProcess_Options options = {};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;

    // 5) 调用 StartNativeChildProcess (在主进程上下文，可以调用多次)
    int32_t childPid = -1;
    int32_t ret = OH_Ability_StartNativeChildProcess(
        const_cast<char*>("libwine_child.so:Main"), args, options, &childPid);

    OH_LOG_INFO(LOG_APP, "[Broker] StartNativeChildProcess ret=%{public}d childPid=%{public}d",
                ret, childPid);

    free(entryParamsCopy);
    // 注意: 所有 fd 的所有权已转移给 StartNativeChildProcess，不要在这里 close

    // 6) 发送响应: childPid + status (8 字节，小端序)
    int32_t response[2];
    response[0] = childPid;  // pid (低 32 位)
    response[1] = ret;       // NCP_ReturnCode

    ssize_t sent = send(conn_fd, response, sizeof(response), MSG_NOSIGNAL);
    if (sent != sizeof(response)) {
        OH_LOG_ERROR(LOG_APP, "[Broker] send response failed: %{public}s", strerror(errno));
    }

    close(conn_fd);
}

void SetBrokerSessionEnv(std::vector<std::string> env)
{
    size_t count = env.size();
    {
        std::lock_guard<std::mutex> lock(gBrokerSessionEnvMutex);
        gBrokerSessionEnv = std::move(env);
    }
    OH_LOG_INFO(LOG_APP, "[Broker] session env set count=%{public}zu", count);
}

void ClearBrokerSessionEnv()
{
    {
        std::lock_guard<std::mutex> lock(gBrokerSessionEnvMutex);
        gBrokerSessionEnv.clear();
    }
    OH_LOG_INFO(LOG_APP, "[Broker] session env cleared");
}

// Broker 线程主循环
static void BrokerThreadFunc()
{
    OH_LOG_INFO(LOG_APP, "[Broker] thread starting");

    // 1) 创建 Unix socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] socket() failed: %{public}s", strerror(errno));
        return;
    }

    // 2) 绑定到已知路径
    unlink(kBrokerSocketPath);  // 清理残留
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kBrokerSocketPath, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] bind(%{public}s) failed: %{public}s",
                     kBrokerSocketPath, strerror(errno));
        close(server_fd);
        return;
    }

    // 3) Listen
    if (listen(server_fd, 8) < 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] listen() failed: %{public}s", strerror(errno));
        close(server_fd);
        return;
    }

    OH_LOG_INFO(LOG_APP, "[Broker] listening on %{public}s", kBrokerSocketPath);

    // 4) Accept 循环
    while (gBrokerRunning.load(std::memory_order_relaxed)) {
        int conn_fd = accept(server_fd, nullptr, nullptr);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            OH_LOG_ERROR(LOG_APP, "[Broker] accept() failed: %{public}s", strerror(errno));
            break;
        }
        // 处理请求（同步：每个请求一个接一个处理）
        HandleRequest(conn_fd);
    }

    close(server_fd);
    unlink(kBrokerSocketPath);
    OH_LOG_INFO(LOG_APP, "[Broker] thread exiting");
}

int StartBrokerServer()
{
    if (gBrokerRunning.load(std::memory_order_acquire)) {
        OH_LOG_WARN(LOG_APP, "[Broker] already running");
        return 0;
    }

    gBrokerRunning.store(true, std::memory_order_release);
    std::thread(BrokerThreadFunc).detach();

    // 等待 broker socket 文件创建 (避免盲等)
    if (!WaitFor("broker socket", []() { return access(kBrokerSocketPath, F_OK) == 0; }, 2000, 50)) {
        OH_LOG_WARN(LOG_APP, "[Broker] socket creation slow, continuing anyway");
    }
    return 0;
}
