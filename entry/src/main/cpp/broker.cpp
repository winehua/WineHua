/**
 * broker.cpp — Process Broker: Unix socket server
 *
 * 在主进程中运行，接收来自 spawn_process (ntdll.so) 的子进程创建请求。
 * 每个请求包含 entryParams 字符串 + 一个 WINESERVERSOCKET fd (SCM_RIGHTS)。
 * Broker 在主进程上下文调用 OH_Ability_StartNativeChildProcess，
 * 从而绕过 appspawn 子进程中无法嵌套调用 NCP API 的限制。
 *
 * 协议 (简单二进制):
 *   请求: "SPAWN\n{entryParams}\n" + SCM_RIGHTS{1 fd}
 *   响应: [childPid: int32_le] [status: int32_le]   (8 字节)
 */
#include "broker.h"
#include "wait_utils.h"
#include "wine_constants.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <AbilityKit/native_child_process.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "WL_Broker"
#include <hilog/log.h>

static const char* kBrokerSocketPath = WINE_BROKER_SOCKET;

static std::atomic<bool> gBrokerRunning{false};

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

    // SCM_RIGHTS 控制消息缓冲区 (接收最多 1 个 fd)
    union {
        char buf[CMSG_SPACE(sizeof(int))];
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

    // 2) 解析 "SPAWN\n{entryParams}\n"
    if (strncmp(buf, "SPAWN\n", 6) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] bad protocol: %{public}s", buf);
        close(conn_fd);
        return;
    }
    char* entryParamsRaw = buf + 6;
    // 去掉末尾的 \n
    size_t elen = strlen(entryParamsRaw);
    while (elen > 0 && entryParamsRaw[elen - 1] == '\n') entryParamsRaw[--elen] = '\0';

    OH_LOG_INFO(LOG_APP, "[Broker] request entryParams=%{public}s", entryParamsRaw);

    // 3) 提取 fd (SCM_RIGHTS)
    int receivedFd = -1;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(&receivedFd, CMSG_DATA(cmsg), sizeof(int));
        OH_LOG_INFO(LOG_APP, "[Broker] received fd=%{public}d via SCM_RIGHTS", receivedFd);
    }

    // 4) 构造 NativeChildProcess 参数
    // 复制 entryParams: StartNativeChildProcess 是异步的，参数需要存活直到调用返回
    char* entryParamsCopy = strdup(entryParamsRaw);

    NativeChildProcess_Fd fdNode = {};
    if (receivedFd >= 0) {
        fdNode.fdName = const_cast<char*>("wineserver_sock");
        fdNode.fd = receivedFd;
        fdNode.next = nullptr;
    }

    NativeChildProcess_FdList fdList = {};
    fdList.head = (receivedFd >= 0) ? &fdNode : nullptr;

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
    // 注意: receivedFd 的所有权已转移给 StartNativeChildProcess，不要在这里 close

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

    // 等待 broker socket 文件创建 (避免盲等)
    if (!WaitFor("broker socket", []() { return access(kBrokerSocketPath, F_OK) == 0; }, 2000, 50)) {
        OH_LOG_WARN(LOG_APP, "[Broker] socket creation slow, continuing anyway");
    }
    return 0;
}
