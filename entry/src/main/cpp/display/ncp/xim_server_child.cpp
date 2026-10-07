/* xim_server_child.cpp — XIM server NCP 子进程 (XIM spec §3 NCP 承载)。
 *
 * 为什么在子进程: X1 探针实测 app 主进程 XOpenDisplay 挂起不返回 (X socket
 * 对主进程 mount namespace 不可达, 2026-10-07, 提交 1392d60); xclient_child
 * 所在的 NCP 子进程与 Xwayland 同 namespace, 连接可行 (xclient_child.cpp
 * /tmp 探针 + sockets.c 标准路径优先补丁)。
 *
 * 入口形态: OH_Ability_StartNativeChildProcess("libxim_server_child.so:Main")
 *   entryParams: "<stderrPath>|<xdgDir>"
 *   fdList: "xim_fd" = socketpair 子端 (主进程 xim_bridge_channel_init 创建,
 *           主端在合成器进程, drain 经它转投 commit 请求)
 * 通道协议: {magic u32 = 0x57485349 ('WHSI'), len u32, utf8 bytes}
 *
 * XIM 注册面 (libX11 1.8.10 ximcp 发现序列, 实测 2026-10-07 三处契约):
 *   1. root 的 XIM_SERVERS property: type=XA_ATOM/format=32, 内容 = selection
 *      atom 数组 (imDefIm.c:_XimPreConnect:403)。条目不是字符串名!
 *   2. selection 名必须字面为 "@server=winehua" (imDefIm.c:_XimCheckServerName
 *      要求 atom 名以 "@server=" 开头再逗号分割匹配 @im 名; 首版把 owner 设在
 *      "XIM_SERVERS" selection 上, client 查列表 atom 的 owner 查不到 →
 *      "Could not open input method")。
 *   3. SelectionRequest 按 target 应答 (property 的 TYPE 必须等于 target atom,
 *      imDefIm.c:_XimGetSelectionNotify:267): "LOCALES"/"TRANSPORT" (注意
 *      atom 名无 XIM_ 前缀, XimProto.h:36-37):
 *      LOCALES   → "@locale=zh_CN.UTF-8,en_US.UTF-8,C" (imDefIm.c:_XimCheckLocaleName)
 *      TRANSPORT → "@transport=X/"  (imTrX.c:_XimXConf 不读地址)
 *   4. wine 侧 XOpenIM (@im=winehua, XMODIFIERS 由 wine_env 下发) 走完发现后
 *      _XimXConnect 发 _XIM_XCONNECT → 握手。
 * 另: guest libX11 需要 XLOCALEDIR 指向打包的 share/X11/locale, 否则
 * XSupportsLocale() 失败, xim_init 在发现之前就放弃 (assemble.sh 打包 +
 * wine_env_baseline.h 下发, 2026-10-07 实测修复)。
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <unistd.h>

#define TAG "xim-server"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define OHERR(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace {

/* 按名取父进程传下的 fd (同 xwayland_child.cpp:44 的 FdByName) */
bool FdByName(const NativeChildProcess_Args &args, const char *name, int *out)
{
    for (NativeChildProcess_Fd *node = args.fdList.head; node; node = node->next)
    {
        if (node->fdName && strcmp(node->fdName, name) == 0)
        {
            *out = node->fd;
            return true;
        }
    }
    return false;
}

Display *g_dpy;
Window g_srv_win;
int g_chan_fd = -1;
std::atomic<bool> g_run{true};

/* ── XIM 协议层 (Task 3) ─────────────────────────────────────────────
 * 常量权威来源: fcitx/xcb-imdkit ximproto.h (opcode) + XimProto.h
 * (XimType, 经 fcitx doc/API.txt 佐证) + exwm-xim.el (能跑通的最小
 * server 参照: OPEN_REPLY 极简属性面/0 版本传输/包编码)。注意计划稿里
 * 记忆值 COMMIT=69/CREATE_IC=20 均错, 以本表为准。 */
#define XIM_CONNECT 1
#define XIM_CONNECT_REPLY 2
#define XIM_DISCONNECT 3
#define XIM_DISCONNECT_REPLY 4
#define XIM_ERROR 20
#define XIM_OPEN 30
#define XIM_OPEN_REPLY 31
#define XIM_CLOSE 32
#define XIM_CLOSE_REPLY 33
#define XIM_SET_EVENT_MASK 37
#define XIM_ENCODING_NEGOTIATION 38
#define XIM_ENCODING_NEGOTIATION_REPLY 39
#define XIM_QUERY_EXTENSION 40
#define XIM_QUERY_EXTENSION_REPLY 41
#define XIM_GET_IM_VALUES 44
#define XIM_GET_IM_VALUES_REPLY 45
#define XIM_CREATE_IC 50
#define XIM_CREATE_IC_REPLY 51
#define XIM_DESTROY_IC 52
#define XIM_DESTROY_IC_REPLY 53
#define XIM_SET_IC_VALUES 54
#define XIM_SET_IC_VALUES_REPLY 55
#define XIM_GET_IC_VALUES 56
#define XIM_GET_IC_VALUES_REPLY 57
#define XIM_SET_IC_FOCUS 58
#define XIM_FORWARD_EVENT 60
#define XIM_SYNC 61
#define XIM_SYNC_REPLY 62
#define XIM_COMMIT 63

#define XIMTYPE_SEPARATOR 0
#define XIMTYPE_CARD8 1
#define XIMTYPE_CARD16 2
#define XIMTYPE_CARD32 3
#define XIMTYPE_WINDOW 5
#define XIMTYPE_XIMSTYLES 10

/* XIM 包头 4 字节: [major:1][minor:1][length:2] (length 单位 4 字节, 网络序) */
#define XIM_CM_DATA_SIZE 20
#define XIM_PROTO_MAJOR 1
#define XIM_PROTO_MINOR 0

struct XimClient
{
    Window xconnect_win = None;  /* client 通信窗口 (回包目标) */
    Window srv_win = None;       /* 本 client 专属 server 窗口 (入站分流键) */
    int lsb = 1;                 /* CONNECT 协商的字节序 (1 = LSB first) */
    bool lsb_known = false;      /* CONNECT 前未知, 首包按启发式解析 */
    uint16_t im_id = 0;
    uint16_t next_ic = 1;
    Window ic_client_win = None; /* XICATTRIBUTE clientWindow (GET_IC_VALUES 回读) */
    bool active = false;
    std::string rx;              /* 入站分片重组 (per-client 流) */
};
/* XIM transport 每 client 一条流: client 的全部协议包发往 XCONNECT 应答
 * l[0] 所指窗口 (imTrX.c: ims_connect_wid), server 按目的窗口分流。首版
 * 全体 client 共用一个窗口+一个重组缓冲: explorer 先连后, 应用进程的
 * CONNECT 分片与 explorer 的流交织, 永远拼不出完整包 → 该进程 XOpenIM
 * 卡死, 窗口不映射 (2026-10-07 实测: graphics_smoke 借道卡死, 画布无窗)。 */
static std::vector<XimClient> g_xims;
static Window g_focus_srv = None; /* ChanThread COMMIT 的目标 (Task 4 接线) */

static XimClient *ClientBySrvWin(Window w)
{
    for (auto &c : g_xims)
        if (c.srv_win == w)
            return &c;
    return nullptr;
}

static void Put16(std::string &b, const XimClient &c, uint16_t v)
{
    if (c.lsb)
    {
        b.push_back((char)(v & 0xff));
        b.push_back((char)(v >> 8));
    }
    else
    {
        b.push_back((char)(v >> 8));
        b.push_back((char)(v & 0xff));
    }
}

static uint16_t Get16(const XimClient &c, const unsigned char *p)
{
    return c.lsb ? (uint16_t)(p[0] | (p[1] << 8))
                 : (uint16_t)((p[0] << 8) | p[1]);
}

static void Put32(std::string &b, const XimClient &c, uint32_t v) /* 按 client 字节序 */
{
    for (int i = 0; i < 4; i++)
    {
        int shift = c.lsb ? (i * 8) : ((3 - i) * 8);
        b.push_back((char)((v >> shift) & 0xff));
    }
}

static void PutStr(std::string &b, const char *s) /* STR: 1 字节长 + 内容 + pad4 */
{
    size_t n = strlen(s);
    b.push_back((char)n);
    b.append(s, n);
    b.append((4 - ((1 + n) % 4)) % 4, '\0');
}

/* 协议包 → transport 发送 (exwm-xim--make-request 同构):
 * ≤20 字节单条 format=8 CM 补零; 更长走 property (XA_STRING, format 8,
 * Append) + format=32 通知 (l[0]=字节数, l[1]=property atom)。 */
static void SendPacket(XimClient &c, uint8_t major, uint8_t minor,
                       const std::string &payload)
{
    size_t words = (payload.size() + 3) / 4;
    std::string pkt;
    pkt.push_back((char)major);
    pkt.push_back((char)minor);
    /* 包头 length 字段 = 协商字节序的 CARD16 (libX11 imDefIm.c:_XimSetHeader
     * 是宿主序对齐存储, 客户端按 CONNECT 协商的序读)。首版恒大端 → lsb
     * 客户端把 1 字读成 256 字, CONNECT_REPLY 永远凑不齐 → explorer 卡死
     * XOpenIM, 桌面根 60s 超时 ready-degraded (2026-10-07 双机 100% 复现,
     * 本修复前的所有会话)。XCONNECT 应答无包头所以此前能通。 */
    if (c.lsb)
    {
        pkt.push_back((char)(words & 0xff));
        pkt.push_back((char)((words >> 8) & 0xff));
    }
    else
    {
        pkt.push_back((char)((words >> 8) & 0xff));
        pkt.push_back((char)(words & 0xff));
    }
    pkt += payload;
    pkt.append(words * 4 - payload.size(), '\0');

    const char *proto = "_XIM_PROTOCOL";
    if (pkt.size() <= XIM_CM_DATA_SIZE)
    {
        char b20[XIM_CM_DATA_SIZE] = {0};
        memcpy(b20, pkt.data(), pkt.size());
        XEvent ev = {};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c.xconnect_win;
        ev.xclient.message_type = XInternAtom(g_dpy, proto, False);
        ev.xclient.format = 8;
        memcpy(ev.xclient.data.b, b20, XIM_CM_DATA_SIZE);
        XSendEvent(g_dpy, c.xconnect_win, False, NoEventMask, &ev);
    }
    else
    {
        char pname[32];
        static unsigned prop_seq; /* 同秒冲突会累积旧数据, 用递增序号 */
        snprintf(pname, sizeof(pname), "_WINEHUA_XIM_%x", ++prop_seq);
        Atom prop = XInternAtom(g_dpy, pname, False);
        XChangeProperty(g_dpy, c.xconnect_win, prop, XA_STRING, 8,
                        PropModeAppend, (const unsigned char *)pkt.data(),
                        (int)pkt.size());
        XEvent ev = {};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c.xconnect_win;
        ev.xclient.message_type = XInternAtom(g_dpy, proto, False);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)pkt.size();
        ev.xclient.data.l[1] = (long)prop;
        XSendEvent(g_dpy, c.xconnect_win, False, NoEventMask, &ev);
    }
}

/* OPEN_REPLY 的属性面 (exwm-xim 极简集):
 * XIMATTR 1 项 = queryInputStyle (id 0, XIMSTYLES);
 * XICATTR 3 项 = inputStyle(0, CARD32) / clientWindow(1, WINDOW) /
 * focusWindow(2, WINDOW)。 */
static std::string BuildOpenReply(const XimClient &c, uint16_t im_id)
{
    std::string b;
    Put16(b, c, im_id);
    /* XIMATTR: [id:2][type:2][nlen:2][name pad4] */
    const char *qis = "queryInputStyle";
    Put16(b, c, 0);
    Put16(b, c, XIMTYPE_XIMSTYLES);
    Put16(b, c, (uint16_t)strlen(qis));
    b.append(qis, strlen(qis));
    b.append((4 - ((2 + strlen(qis)) % 4)) % 4, '\0');
    /* XICATTR */
    const char *names[3] = {"inputStyle", "clientWindow", "focusWindow"};
    uint16_t types[3] = {XIMTYPE_CARD32, XIMTYPE_WINDOW, XIMTYPE_WINDOW};
    for (int i = 0; i < 3; i++)
    {
        Put16(b, c, (uint16_t)i);
        Put16(b, c, types[i]);
        Put16(b, c, (uint16_t)strlen(names[i]));
        b.append(names[i], strlen(names[i]));
        b.append((4 - ((2 + strlen(names[i])) % 4)) % 4, '\0');
    }
    return b;
}

/* 入站重组: 客户端 >20 字节的包 (SET_IC_VALUES 等) 分片为多条 format=8 CM,
 * 按包头 length (协商字节序, 与 SendPacket 同一约定) 累积拼包;
 * format=32 property 通知自带完整包, 也走同一条口。CONNECT 时清空。 */
static void HandlePacket(XimClient &c, const std::string &pkt);

static void RxFeed(XimClient &c, const char *data, size_t n)
{
    c.rx.append(data, n);
    while (c.rx.size() >= 4)
    {
        uint16_t le = (uint16_t)((uint8_t)c.rx[2] | ((uint16_t)(uint8_t)c.rx[3] << 8));
        uint16_t be = (uint16_t)(((uint16_t)(uint8_t)c.rx[2] << 8) | (uint8_t)c.rx[3]);
        uint16_t words;
        if (!c.lsb_known)
        {
            /* CONNECT 包: 客户端字节序未知, 取「总长不超过已收字节」的读法。
             * 首包 ≤20B 单 CM 已到齐; wine x86 = LE, 其 length=2 字按 LE 读
             * 得 2 (可行), 按 BE 读得 512 (需 2052 字节, 永远凑不齐 → 曾把
             * explorer 卡死在 XOpenIM, 桌面根 60s 超时, 2026-10-07 实测)。 */
            words = (4 + (size_t)le * 4 <= c.rx.size()) ? le : be;
        }
        else
        {
            words = c.lsb ? le : be;
        }
        size_t total = 4 + (size_t)words * 4;
        if (total <= 4)
            break; /* 防御: 0 字长声明避免死循环 */
        if (c.rx.size() < total)
            break;
        HandlePacket(c, c.rx.substr(0, total));
        c.rx.erase(0, total);
    }
}

static void HandlePacket(XimClient &c, const std::string &pkt)
{
    if (pkt.size() < 4)
        return;
    uint8_t major = (uint8_t)pkt[0];
    uint8_t minor = (uint8_t)pkt[1];
    const unsigned char *p = (const unsigned char *)pkt.data() + 4;

    switch (major)
    {
    case XIM_CONNECT:
    {
        /* 载荷: byte-order(1, 'l'/'B') + pad + client-major:2 client-minor:2
         * (exwm: data[4] 即字节序标志) */
        c.lsb = (pkt.size() > 4 && pkt[4] == 'l');
        c.im_id = 0;
        c.next_ic = 1;
        c.ic_client_win = None;
        c.active = true;
        c.lsb_known = true;
        c.rx.clear(); /* 新协商: 丢弃旧流残留分片 */
        std::string b;
        Put16(b, c, XIM_PROTO_MAJOR);
        Put16(b, c, XIM_PROTO_MINOR);
        SendPacket(c, XIM_CONNECT_REPLY, 0, b);
        fprintf(stderr, "[xim-server] CONNECT lsb=%d\n", c.lsb);
        break;
    }
    case XIM_OPEN:
    {
        c.im_id = (uint16_t)(c.im_id + 1);
        SendPacket(c, XIM_OPEN_REPLY, 0, BuildOpenReply(c, c.im_id));
        /* on-spot None 风格不需要键事件转发 */
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, 0); /* ic-id 0 = IM 级 */
        Put16(b, c, 0); /* forward-event-mask = 0 */
        Put16(b, c, 0); /* synchronous-event-mask = 0 */
        SendPacket(c, XIM_SET_EVENT_MASK, 0, b);
        fprintf(stderr, "[xim-server] OPEN im_id=%u locale-first=%d\n",
                c.im_id, pkt.size() > 4 ? (int)p[0] : -1);
        break;
    }
    case XIM_GET_IM_VALUES:
    {
        /* [im-id:2][n:2][attr-id:2...] — 只认 queryInputStyle (id 0)。
         * REPLY: [im-id:2][length:2 字数] + XIMATTRIBUTE[id:2][vlen:2][value]，
         * XIMSTYLES 值 = [n:2][CARD32×n] (字节序随 client)。 */
        uint16_t n = pkt.size() >= 8 ? Get16(c, p + 2) : 0;
        bool only_style = (n == 1 && pkt.size() >= 10 && Get16(c, p + 4) == 0);
        std::string b;
        Put16(b, c, c.im_id);
        std::string attr;
        if (only_style)
        {
            std::string val;
            Put16(val, c, 1); /* 1 组 style */
            Put32(val, c, 0x0808); /* PreeditNothing | StatusNothing */
            Put16(attr, c, 0); /* queryInputStyle 的 attr id */
            Put16(attr, c, (uint16_t)val.size());
            attr += val;
            attr.append((4 - (val.size() % 4)) % 4, '\0');
        }
        Put16(b, c, (uint16_t)(attr.size() / 4));
        b += attr;
        SendPacket(c, XIM_GET_IM_VALUES_REPLY, 0, b);
        fprintf(stderr, "[xim-server] GET_IM_VALUES n=%u only_style=%d\n", n,
                only_style);
        break;
    }
    case XIM_CREATE_IC:
    {
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, c.next_ic);
        SendPacket(c, XIM_CREATE_IC_REPLY, 0, b);
        c.next_ic++;
        fprintf(stderr, "[xim-server] CREATE_IC\n");
        break;
    }
    case XIM_SET_IC_VALUES:
    {
        /* XICATTRIBUTE 列表: [id:2][vlen:2][value][pad4] — 抓 clientWindow */
        size_t off = 4;
        while (off + 4 <= pkt.size())
        {
            uint16_t id = Get16(c, p + off);
            uint16_t vlen = Get16(c, p + off + 2);
            if (id == 1 && vlen == sizeof(Window) &&
                off + 4 + vlen <= pkt.size())
            {
                memcpy(&c.ic_client_win, p + off + 4, sizeof(Window));
                fprintf(stderr, "[xim-server] SET_IC_VALUES clientWindow=0x%lx\n",
                        (unsigned long)c.ic_client_win);
            }
            off += 4 + vlen + ((4 - (vlen % 4)) % 4);
        }
        std::string b;
        Put16(b, c, c.im_id);
        SendPacket(c, XIM_SET_IC_VALUES_REPLY, 0, b);
        break;
    }
    case XIM_GET_IC_VALUES:
    {
        /* [im-id:2][n:2][attr-id:2...] — 逐一回值 */
        uint16_t n = pkt.size() >= 8 ? Get16(c, p + 2) : 0;
        std::string b;
        Put16(b, c, c.im_id);
        std::string vals;
        for (uint16_t i = 0; i < n && 6 + (size_t)i * 2 <= pkt.size(); i++)
        {
            uint16_t id = Get16(c, p + 4 + (size_t)i * 2);
            if (id == 0)
            { /* inputStyle CARD32 */
                Put16(vals, c, 0);
                Put16(vals, c, 4);
                Put32(vals, c, 0x0808);
            }
            else if (id == 1)
            { /* clientWindow */
                Put16(vals, c, 1);
                Put16(vals, c, (uint16_t)sizeof(Window));
                vals.append((const char *)&c.ic_client_win, sizeof(Window));
            }
            else if (id == 2)
            { /* focusWindow = 同 clientWindow */
                Put16(vals, c, 2);
                Put16(vals, c, (uint16_t)sizeof(Window));
                vals.append((const char *)&c.ic_client_win, sizeof(Window));
            }
        }
        Put16(b, c, (uint16_t)vals.size());
        b += vals;
        SendPacket(c, XIM_GET_IC_VALUES_REPLY, 0, b);
        fprintf(stderr, "[xim-server] GET_IC_VALUES n=%u\n", n);
        break;
    }
    case XIM_SYNC:
    {
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, 0);
        SendPacket(c, XIM_SYNC_REPLY, 0, b);
        break;
    }
    case XIM_DISCONNECT:
        SendPacket(c, XIM_DISCONNECT_REPLY, 0, std::string());
        c.active = false;
        fprintf(stderr, "[xim-server] DISCONNECT\n");
        break;
    default:
        fprintf(stderr, "[xim-server] unhandled major=%u\n", major);
        break;
    }
}

/* socket 读线程: 主进程转投的 commit 请求 (Task 4 接 XIM_COMMIT 发送)。 */
void *ChanThread(void *arg)
{
    (void)arg;
    uint32_t hdr[2];
    while (g_run.load())
    {
        ssize_t n = read(g_chan_fd, hdr, sizeof(hdr));
        if (n == 0)
            break; /* 主端关闭 */
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n != sizeof(hdr) || hdr[0] != 0x57485349u)
        {
            OHERR("chan: bad header n=%{public}zd magic=%{public}u", n, hdr[0]);
            continue;
        }
        std::string text(hdr[1], '\0');
        size_t off = 0;
        while (off < hdr[1])
        {
            n = read(g_chan_fd, &text[off], hdr[1] - off);
            if (n <= 0)
                return NULL;
            off += (size_t)n;
        }
        fprintf(stderr, "[xim-server] chan text len=%u (COMMIT 接线 Task 4)\n",
                hdr[1]);
    }
    OHLOG("chan: closed");
    return NULL;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char *params = args.entryParams ? args.entryParams : "";
    /* NCP 子进程的 hilog LOG_APP 不可见 (Task 2 实测: spawn 后 0 行), 关键
     * 状态走 fprintf(stderr)——stderr 已重定向到 xim_server_stderr.log。 */
    fprintf(stderr, "[xim-server] Main enter pid=%d params=%s\n", getpid(), params);
    OHLOG("Main enter pid=%{public}d params=%{public}s", getpid(), params);

    /* entryParams: "<stderrPath>|<xdgDir>" (同 xclient_child 解析) */
    std::string entryParams(params);
    std::string parts[2];
    size_t start = 0;
    for (int i = 0; i < 2; ++i)
    {
        size_t bar = entryParams.find('|', start);
        parts[i] = bar == std::string::npos ? entryParams.substr(start)
                                            : entryParams.substr(start, bar - start);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (!parts[0].empty())
    {
        int logFd = open(parts[0].c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logFd >= 0)
        {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) close(logFd);
        }
    }
    if (!parts[1].empty())
        setenv("XDG_RUNTIME_DIR", parts[1].c_str(), 1);

    if (!FdByName(args, "xim_fd", &g_chan_fd) || g_chan_fd < 0)
    {
        fprintf(stderr, "[xim-server] missing xim_fd, aborting\n");
        OHERR("missing xim_fd, aborting");
        return;
    }
    fprintf(stderr, "[xim-server] chan fd=%d\n", g_chan_fd);
    OHLOG("chan fd ready fd=%{public}d", g_chan_fd);

    g_dpy = XOpenDisplay(":0");
    if (!g_dpy)
    {
        fprintf(stderr, "[xim-server] XOpenDisplay FAILED\n");
        OHERR("XOpenDisplay FAILED (namespace 与 xclient 不同?)");
        return;
    }
    fprintf(stderr, "[xim-server] X connected vendor=%s\n", XServerVendor(g_dpy));
    OHLOG("X connected vendor=%{public}s", XServerVendor(g_dpy));

    /* XIM 注册面: property 列 selection atom; selection 名字面 "@server=winehua"
     * (_XimCheckServerName 按此名匹配 @im=winehua; 见文件头契约 1/2) */
    Window root = DefaultRootWindow(g_dpy);
    Atom servers = XInternAtom(g_dpy, "XIM_SERVERS", False);
    Atom imsel = XInternAtom(g_dpy, "@server=winehua", False);
    XChangeProperty(g_dpy, root, servers, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&imsel, 1);
    XSetWindowAttributes swa = {};
    g_srv_win = XCreateWindow(g_dpy, root, -1, -1, 1, 1, 0, CopyFromParent,
                              InputOnly, CopyFromParent, CWEventMask, &swa);
    XSetSelectionOwner(g_dpy, imsel, g_srv_win, CurrentTime);
    if (XGetSelectionOwner(g_dpy, imsel) != g_srv_win)
    {
        fprintf(stderr, "[xim-server] selection ownership FAILED\n");
        OHERR("selection ownership FAILED");
        return;
    }
    fprintf(stderr, "[xim-server] registered @server=winehua owner=0x%lx\n",
            (unsigned long)g_srv_win);
    OHLOG("registered XIM_SERVERS owner=0x%{public}lx",
          (unsigned long)g_srv_win);

    pthread_t chan;
    if (pthread_create(&chan, NULL, ChanThread, NULL) == 0)
        pthread_detach(chan);

    /* X 事件循环: SelectionRequest (发现面) + ClientMessage (transport:
     * _XIM_XCONNECT 应答 / _XIM_PROTOCOL 收包)。 */
    Atom aProtocol = XInternAtom(g_dpy, "_XIM_PROTOCOL", False);
    Atom aXConnect = XInternAtom(g_dpy, "_XIM_XCONNECT", False);
    /* 发现面 target (XimProto.h:36-37 — atom 名无 XIM_ 前缀); 应答 property
     * 的 TYPE 必须 = target atom (_XimGetSelectionNotify 按类型取) */
    Atom aLocales = XInternAtom(g_dpy, "LOCALES", False);
    Atom aTransport = XInternAtom(g_dpy, "TRANSPORT", False);
    while (g_run.load())
    {
        while (XPending(g_dpy))
        {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            if (ev.type == SelectionRequest)
            {
                XSelectionRequestEvent *req = &ev.xselectionrequest;
                XSelectionEvent sev = {};
                sev.type = SelectionNotify;
                sev.display = g_dpy;
                sev.requestor = req->requestor;
                sev.selection = req->selection;
                sev.target = req->target;
                sev.property = req->property;
                sev.time = CurrentTime;
                const char *val;
                Atom type;
                if (req->target == aLocales)
                {
                    val = "@locale=zh_CN.UTF-8,en_US.UTF-8,C";
                    type = aLocales;
                }
                else if (req->target == aTransport)
                {
                    val = "@transport=X/";
                    type = aTransport;
                }
                else
                {
                    val = "winehua";
                    type = XA_STRING;
                }
                XChangeProperty(g_dpy, req->requestor, req->property, type, 8,
                                PropModeReplace,
                                (const unsigned char *)val, (int)strlen(val));
                XSendEvent(g_dpy, req->requestor, False, 0, (XEvent *)&sev);
                fprintf(stderr, "[xim-server] selection served target=%s req=0x%lx\n",
                        XGetAtomName(g_dpy, req->target),
                        (unsigned long)req->requestor);
            }
            else if (ev.type == ClientMessage)
            {
                if (ev.xclient.message_type == aXConnect)
                {
                    /* 首连 CM 到达主窗口 (selection 发现的窗口)。每 client
                     * 建专属 server 窗口并在应答 l[0] 告知 — 此后该 client
                     * 的全部协议包都发往它, 按目的窗口分流 (见 g_xims 注)。 */
                    XimClient c;
                    c.xconnect_win = (Window)ev.xclient.data.l[0];
                    c.srv_win = XCreateWindow(g_dpy, root, -1, -1, 1, 1, 0,
                                              CopyFromParent, InputOnly,
                                              CopyFromParent, CWEventMask,
                                              &swa);
                    g_xims.push_back(c);
                    g_focus_srv = c.srv_win;
                    XEvent rep = {};
                    rep.xclient.type = ClientMessage;
                    rep.xclient.window = c.xconnect_win;
                    rep.xclient.message_type = aXConnect;
                    rep.xclient.format = 32;
                    /* 应答 (l[0]=本 client 专属 server win, 0,0, dividing=0)
                     * — 0/0 = only-CM & Property-with-CM (Table 4-2)。 */
                    rep.xclient.data.l[0] = (long)c.srv_win;
                    rep.xclient.data.l[1] = 0;
                    rep.xclient.data.l[2] = 0;
                    rep.xclient.data.l[3] = 0;
                    XSendEvent(g_dpy, c.xconnect_win, False, NoEventMask, &rep);
                    fprintf(stderr, "[xim-server] XCONNECT client=0x%lx srv=0x%lx\n",
                            (unsigned long)c.xconnect_win,
                            (unsigned long)c.srv_win);
                }
                else if (ev.xclient.message_type == aProtocol)
                {
                    XimClient *c = ClientBySrvWin(ev.xclient.window);
                    if (!c)
                    {
                        fprintf(stderr,
                                "[xim-server] CM from unknown srv=0x%lx\n",
                                (unsigned long)ev.xclient.window);
                    }
                    else if (ev.xclient.format == 8)
                    {
                        /* 分片: 20 字节恒定, 重组按包头 length 累积 (RxFeed) */
                        RxFeed(*c, ev.xclient.data.b, XIM_CM_DATA_SIZE);
                    }
                    else if (ev.xclient.format == 32)
                    {
                        /* Property-with-CM: l[0]=字节数, l[1]=property atom,
                         * delete=True 读走 (Transport Table 4-7) */
                        Atom prop = (Atom)ev.xclient.data.l[1];
                        unsigned long n = 0, left = 0;
                        Atom type = None;
                        int fmt = 0;
                        unsigned char *data = NULL;
                        if (XGetWindowProperty(g_dpy, c->xconnect_win, prop,
                                               0, 0x10000, True, XA_STRING,
                                               &type, &fmt, &n, &left,
                                               &data) == Success && data)
                        {
                            fprintf(stderr, "[xim-server] prop packet %lu bytes\n",
                                    n);
                            RxFeed(*c, (const char *)data, n);
                            XFree(data);
                        }
                    }
                }
            }
        }
        usleep(5 * 1000); /* 事件泵 200Hz (XIM 握手/commit 时延敏感) */
    }
}
