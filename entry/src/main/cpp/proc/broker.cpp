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
#include "common/wait_utils.h"
#include "wine/wine_constants.h"
#include "audio/audio_broker.h"
#include "wine_process.h"
// 由 LaunchPadMode 在启动 Broker 前设置
std::string gBrokerHomeDir;
std::string gBrokerPrefixDir;

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
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

// -- 从 entryParams 解析进程 exe 路径 (登记到任务列表用) --
// broker 加了 homeDir 前缀后 entryParams 形如
//   "homeDir|binDir|[wine]|argv0|argv1|...|__env=K=V|..."
// 或 desktop 标记路径。跳过 homeDir/binDir (前两个 '/' 段) 与
// wine/__winehua_desktop__ 标记段, 取第一个可执行段的完整形式 (Windows 路径
// C:\... / native 绝对路径; AddProcess 自取 basename 作显示名)。早期版本只存
// basename, 任务列表拿不到真实路径, 无法按路径匹配应用库图标或按需提取图标。
// 取不到时回退 "wine"。
static std::string ParseProcessPath(const char* entryParams) {
    std::string path = "wine";
    const std::string params = entryParams ? entryParams : "";
    size_t pos = 0;
    int slashSegsLeft = 2;  // homeDir + binDir, 均以 '/' 开头
    while (pos < params.size()) {
        size_t end = params.find('|', pos);
        std::string seg = params.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!seg.empty()) {
            if (seg.rfind("__env=", 0) == 0) break;  // env 段结束 argv
            if (seg[0] == '/' && slashSegsLeft > 0) { --slashSegsLeft; }
            else if (seg == "wine" || seg == "__winehua_desktop__") { /* 标记段 */ }
            else {
                path = seg;
                break;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return path;
}

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
        // n==0: 对端 connect 后立即 close (StartBrokerServer 的就绪探测),
        // 属正常探测流量, 非错误; n<0 才是真的 recvmsg 失败
        if (n == 0)
            OH_LOG_INFO(LOG_APP, "[Broker] probe connection (readiness check), ignoring");
        else
            OH_LOG_ERROR(LOG_APP, "[Broker] recvmsg failed: %{public}s", strerror(errno));
        close(conn_fd);
        return;
    }
    buf[n] = '\0';

    // 2) 解析 "SPAWN\n{entryParams}\n[FDS:name0,name1,...\n]"
    //    entryParams 到第一个 '\n' 为止; 其后是可选段: FDS: (逗号分隔 fd 名)。
    //    环境变量已序列化为 |__env=K=V| 段嵌入 entryParams, 无需额外解析。
    if (strncmp(buf, "SPAWN\n", 6) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] bad protocol: %{public}s", buf);
        close(conn_fd);
        return;
    }
    char* entryParamsRaw = buf + 6;
    char* fdsLine = nullptr;
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
                }
            }
        }
    }

    OH_LOG_INFO(LOG_APP, "[Broker] request entryParams=%{public}s fds=%{public}s",
                entryParamsRaw, fdsLine ? fdsLine : "(none)");

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

    // 5) 构造 NativeChildProcess 参数。
    // Wine 服务进程会把创建者的环境重新序列化给 broker，但 NCP 不会继承
    // LaunchPad 的环境。把会话 prefix 放在最后 (后写胜出), 覆盖 entryParams
    // 里可能残留的默认 .wine 值。
    std::string fullParams = gBrokerHomeDir.empty() ? entryParamsRaw
                            : (gBrokerHomeDir + "|" + entryParamsRaw);
    if (!gBrokerPrefixDir.empty())
        fullParams += "|__env=WINEPREFIX=" + gBrokerPrefixDir;
    OH_LOG_INFO(LOG_APP, "[Broker] dispatch prefix=%{public}s",
                gBrokerPrefixDir.empty() ? "(inherited)" : gBrokerPrefixDir.c_str());
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

    if (ret == 0 && childPid > 0) {
        // 全量登记: App 侧主动启动 (SpawnViaBroker) 与 wine 内部自启
        // (ohos_broker_spawn_child / loader.c 自启 wineserver) 都汇到 broker。
        // 统一登记使 explorer 里双击的 exe 出现在任务列表; App 侧调用者随后
        // 会用更准确的路径 AddProcess 覆盖 (AddProcess 同 pid 幂等)。
        AddProcess(childPid, ParseProcessPath(fullParams.c_str()), -1);
    }

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
    strcpy(addr.sun_path, kBrokerSocketPath);

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
    }
    return 0;
}
