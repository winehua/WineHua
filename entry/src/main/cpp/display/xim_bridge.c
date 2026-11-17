/* xim_bridge.c — XIM 桥主进程侧 (通道客户端, XIM spec §3 NCP 承载)。
 *
 * 架构 (X1 实测修正, 2026-10-07): app 主进程 XOpenDisplay 挂起不返回 (X
 * socket 对主进程 mount namespace 不可达, 提交 1392d60), XIM server 跑在
 * NCP 子进程 (display/ncp/xim_server_child.cpp, 与 Xwayland 同 namespace,
 * 连接面已证)。本文件只管主进程侧通道:
 *   xim_bridge_channel_init() — socketpair, 子端交 spawn 传给子进程
 *   xim_bridge_send_text()    — drain 线程转投 commit 请求
 * 通道协议: {magic u32 = 0x57485349 ('WHSI'), len u32, utf8 bytes}
 */
#include "xim_bridge.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <hilog/log.h>

#define TAG "xim-bridge"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define OHERR(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

#define XIM_CHAN_MAGIC 0x57485349u

static void StartActivationReader(void); /* 前向: channel_init 与注册处共用 */

static int g_chan_fd = -1;      /* 主端; 子端所有权随 spawn 转移 */
static pthread_mutex_t g_chan_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 上行激活 (x11 IME 上行链): server → 主进程 → ArkTS attach/detach */
static xim_bridge_activation_cb g_act_cb = 0;
static void *g_act_user = 0;
static int g_reader_started = 0;
static pthread_t g_reader;
static int g_active = 0;
static int g_armed = 0;

void xim_bridge_set_armed(int armed)
{
    pthread_mutex_lock(&g_chan_mutex);
    g_armed = !!armed;
    pthread_mutex_unlock(&g_chan_mutex);
}

int xim_bridge_is_active(void)
{
    pthread_mutex_lock(&g_chan_mutex);
    int active = g_chan_fd >= 0 && g_armed && g_active;
    pthread_mutex_unlock(&g_chan_mutex);
    return active;
}

int xim_bridge_channel_init(void)
{
    xim_bridge_shutdown();
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    {
        OHERR("socketpair failed errno=%{public}d", errno);
        return -1;
    }
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    pthread_mutex_lock(&g_chan_mutex);
    g_chan_fd = sv[0];
    pthread_mutex_unlock(&g_chan_mutex);
    OHLOG("channel paired main_fd=%{public}d child_fd=%{public}d", sv[0], sv[1]);
    StartActivationReader();
    return sv[1]; /* 调用方 (spawn) 持有并转移所有权 */
}

/* 全量写: MSG_NOSIGNAL + 短写/EINTR 循环。
 * MSG_NOSIGNAL 是硬需求 (2026-10-13 加固): 对端 (XIM 子进程) 已死时, 裸
 * write 会发 SIGPIPE 把**主进程直接写死** (默认信号动作, cppcrash —— 表现
 * 是"用户打字时应用闪退"); 带上它以 EPIPE 失败返回即可。消息小且互斥串行,
 * 不引入 NONBLOCK/队列 —— 失败即丢本次 commit 并报错。 */
static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int xim_bridge_send_text(const char *utf8)
{
    if (!utf8)
        return -1;
    size_t len = strlen(utf8);
    if (len == 0 || len > 64 * 1024)
        return -1;

    uint32_t hdr[2] = {XIM_CHAN_MAGIC, (uint32_t)len};
    pthread_mutex_lock(&g_chan_mutex);
    int ok = g_chan_fd >= 0 && write_all(g_chan_fd, hdr, sizeof(hdr)) == 0 &&
             write_all(g_chan_fd, utf8, len) == 0;
    // A partial record cannot be resumed as a new message. Invalidate it.
    if (!ok && g_chan_fd >= 0) shutdown(g_chan_fd, SHUT_RDWR);
    pthread_mutex_unlock(&g_chan_mutex);
    if (!ok)
    {
        OHERR("send failed errno=%{public}d (peer gone? EPIPE=32)", errno);
        return -1;
    }
    return 0;
}

/* ---- 上行激活通知 (x11 IME 上行链, 2026-10-17) ----
 * server 帧: {magic u32, len u32 = 20, subtype u32 (1=激活 2=失活),
 * rect[4] i32 (v1 传零)}。与下行共用 socketpair (全双工), 读线程独占读端。
 * server 死亡 (EOF/EPIPE) 视为失活通知一次, 随后读线程退出。 */

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0)
    {
        ssize_t n = recv(fd, p, len, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* EOF: server 已退出 */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *ActivationReaderMain(void *arg)
{
    (void)arg;
    int fd;
    xim_bridge_activation_cb cb;
    void *user;
    pthread_mutex_lock(&g_chan_mutex);
    fd = g_chan_fd;
    cb = g_act_cb;
    user = g_act_user;
    pthread_mutex_unlock(&g_chan_mutex);

    for (;;)
    {
        uint32_t hdr[2];
        uint32_t payload[5]; /* subtype + rect[4] */
        if (read_full(fd, hdr, sizeof(hdr)) != 0)
            break;
        if (hdr[0] != XIM_CHAN_MAGIC || hdr[1] != sizeof(payload))
        {
            OHERR("upstream bad frame magic=0x%{public}x len=%{public}u, drop",
                  hdr[0], hdr[1]);
            break;
        }
        if (read_full(fd, payload, sizeof(payload)) != 0)
            break;
        pthread_mutex_lock(&g_chan_mutex);
        if (fd == g_chan_fd) g_active = payload[0] == 1;
        cb = g_act_cb;
        user = g_act_user;
        pthread_mutex_unlock(&g_chan_mutex);
        if (cb)
            cb(payload[0] == 1 ? 1 : 0,
               (int)payload[1], (int)payload[2],
               (int)payload[3], (int)payload[4], user);
    }

    pthread_mutex_lock(&g_chan_mutex);
    if (fd == g_chan_fd) g_active = 0;
    pthread_mutex_unlock(&g_chan_mutex);
    OHERR("upstream reader exit (server gone?) — notify deactivate");
    if (cb)
        cb(0, 0, 0, 0, 0, user);
    return NULL;
}

static void StartActivationReader(void)
{
    pthread_mutex_lock(&g_chan_mutex);
    if (g_chan_fd >= 0 && g_act_cb && !g_reader_started) {
        if (pthread_create(&g_reader, NULL, ActivationReaderMain, NULL) == 0)
            g_reader_started = 1;
        else OHERR("activation reader thread create failed");
    }
    pthread_mutex_unlock(&g_chan_mutex);
}

// Called on compositor teardown before a replacement channel can be created.
// shutdown wakes recv; join prevents stale activation from entering a new session.
void xim_bridge_shutdown(void)
{
    pthread_mutex_lock(&g_chan_mutex);
    int fd = g_chan_fd;
    int join = g_reader_started;
    g_chan_fd = -1;
    g_active = 0;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    pthread_mutex_unlock(&g_chan_mutex);
    if (join) pthread_join(g_reader, NULL);
    if (fd >= 0) close(fd);
    pthread_mutex_lock(&g_chan_mutex);
    g_reader_started = 0;
    pthread_mutex_unlock(&g_chan_mutex);
}

void xim_bridge_set_activation_callback(xim_bridge_activation_cb cb, void *user)
{
    pthread_mutex_lock(&g_chan_mutex);
    g_act_cb = cb;
    g_act_user = user;
    pthread_mutex_unlock(&g_chan_mutex);
    StartActivationReader();
}
