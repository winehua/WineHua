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
#include <X11/Xutil.h> /* Xutf8TextListToTextProperty (Task 4 CT 转换) */

#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <locale.h> /* setlocale: Xlib CT 转换按 locale (Task 4) */
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
#define XIMTYPE_NEST 0x7fff /* XimProto.h:166 (preedit/statusAttributes) */

#define XIMTYPE_SEPARATOR 0
#define XIMTYPE_CARD8 1
#define XIMTYPE_CARD16 2
#define XIMTYPE_CARD32 3
#define XIMTYPE_WINDOW 5
#define XIMTYPE_XIMSTYLES 10
#define XIMTYPE_XPOINT 12
#define XIMTYPE_XFONTSET 13

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
    uint16_t ic_id = 0;          /* 当前 IC (SET/GET_IC_VALUES 回执须带回) */
    bool ic_focus = false;       /* XIM_SET_IC_FOCUS 实时维护 (commit 定位①) */
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

/* 协议包 → transport 发送 (CM 分片, 标准路径):
 * ≤20 字节单条 format=8 CM 补零; 更长按 20 字节分片连发多条 format=8 CM
 * (首片带 XIM 头, client 按包头 length 重组 —— 与 server 侧 RxFeed 对称;
 * libX11 imTrX.c _XimXRead 同规则)。曾用 property+CM(32) 通知通道
 * (exwm-xim 同构), 但 OPEN_REPLY 经该通道后 client 侧未见后续 —— wine 的
 * XOpenIM 恒停在 negotiation (2026-10-08 实测), 分片 CM 同时排除 property
 * 写入/读取双端的一切疑点。 */
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

    Atom proto = XInternAtom(g_dpy, "_XIM_PROTOCOL", False);
    size_t off = 0;
    while (off < pkt.size())
    {
        XEvent ev = {};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c.xconnect_win;
        ev.xclient.message_type = proto;
        ev.xclient.format = 8;
        size_t chunk = pkt.size() - off;
        if (chunk > XIM_CM_DATA_SIZE)
            chunk = XIM_CM_DATA_SIZE;
        memcpy(ev.xclient.data.b, pkt.data() + off, chunk);
        XSendEvent(g_dpy, c.xconnect_win, False, NoEventMask, &ev);
        off += chunk;
    }
    /* XFlush 保证回包立即出网 (否则攒在输出缓冲等下一轮 pump flush——
     * 客户端阻塞等待时, 5ms 的泵间延迟不应叠加 #101 排查变量) */
    XFlush(g_dpy);
    fprintf(stderr, "[xim-server] -> major=%u len=%zu win=0x%lx\n",
            major, pkt.size(), (unsigned long)c.xconnect_win);
}

/* OPEN_REPLY 载荷布局 (2026-10-08 逐字节实锤, 权威 = client 解析器
 * imRmAttr.c:_XimGetAttributeID + imTransR.c:_XimCountNumberOfAttr):
 *   [im-id:2]
 *   [CARD16: IMATTR 段条目总字节数][entries...]      ← total 是字节数不是
 *   [CARD16: ICATTR 段条目总字节数][CARD16: 跳过][entries]  条目数! client
 *   按 `while (total > 6) total -= entry_len` 走段; IC 段头多一个跳过的
 *   CARD16 (entries 从 &buf[2] 起, imRmAttr.c:1482)。
 * 旧版缺两个段长前缀 → client 取到 buf[0]=首条目 id=0 → n=0 → False →
 * _XimOpen 失败 → XOpenIM 弃连重试 (第二条 CONNECT 后死), nego 永远
 * 发不出来 —— 四轮「死于 negotiation」的误诊根因即此。
 * 属性面 (exwm-xim 极简集): XIMATTR 1 项 = queryInputStyle (id 0, XIMSTYLES);
 * XICATTR 16 项 = wine XCreateIC 实际传参全集 (逐名对 imRm.c 资源表 +
 * xim.c xic_create 调用点核对, 2026-10-08)。条目:
 * [id:2][type:2][nlen:2][name][pad4(nlen+2)]。 */
static void AppendAttrEntry(std::string &b, const XimClient &c, uint16_t id,
                            uint16_t type, const char *name)
{
    Put16(b, c, id);
    Put16(b, c, type);
    Put16(b, c, (uint16_t)strlen(name));
    b.append(name, strlen(name));
    b.append((4 - ((2 + strlen(name)) % 4)) % 4, '\0');
}

static std::string BuildOpenReply(const XimClient &c, uint16_t im_id)
{
    std::string b;
    Put16(b, c, im_id);
    std::string im_attrs;
    AppendAttrEntry(im_attrs, c, 0, XIMTYPE_XIMSTYLES, "queryInputStyle");
    Put16(b, c, (uint16_t)im_attrs.size());
    b += im_attrs;
    std::string ic_attrs;
    AppendAttrEntry(ic_attrs, c, 0, XIMTYPE_CARD32, "inputStyle");
    AppendAttrEntry(ic_attrs, c, 1, XIMTYPE_WINDOW, "clientWindow");
    AppendAttrEntry(ic_attrs, c, 2, XIMTYPE_WINDOW, "focusWindow");
    /* wine 的 XCreateIC 传 preeditAttributes/statusAttributes (NEST, 内含
     * FontSet/spot/callbacks) —— 未声明则 _XimEncodeICATTRIBUTE 走
     * "return p->name" (imRmAttr.c:1120-1124, 非 inner 即失败) → XCreateIC
     * 返回 NULL 且 CREATE_IC 从不发 (2026-10-08 实测 "created XIC 0",
     * server 无 CREATE_IC), COMMIT 因无 IC 丢弃 → 中文不上屏。
     * 嵌套列表「内层」属性走同一张 ICATTR 表查名 (imRmAttr.c:1196 递归同
     * res_list), fontSet/spotLocation/8 个 preedit/status 回调都必须声明;
     * 只有顶层 destroyCallback 在 ic_inner_resources 白名单 (imRm.c:1526)
     * 免声明。callbacks 按惯例声明 WINDOW (wire 编码 CARD32,
     * _XimValueToAttribute 的 Window 分支), fontSet=XFontSet (base font
     * 名单串), spotLocation=XPoint。separatorOfNestedList 规范要求在列
     * (xim.xml:1399-1400)。 */
    AppendAttrEntry(ic_attrs, c, 3, XIMTYPE_NEST, "preeditAttributes");
    AppendAttrEntry(ic_attrs, c, 4, XIMTYPE_NEST, "statusAttributes");
    AppendAttrEntry(ic_attrs, c, 5, XIMTYPE_NEST,
                    "separatorOfNestedList");
    AppendAttrEntry(ic_attrs, c, 6, XIMTYPE_XFONTSET, "fontSet");
    AppendAttrEntry(ic_attrs, c, 7, XIMTYPE_XPOINT, "spotLocation");
    AppendAttrEntry(ic_attrs, c, 8, XIMTYPE_WINDOW,
                    "preeditStartCallback");
    AppendAttrEntry(ic_attrs, c, 9, XIMTYPE_WINDOW, "preeditDoneCallback");
    AppendAttrEntry(ic_attrs, c, 10, XIMTYPE_WINDOW, "preeditDrawCallback");
    AppendAttrEntry(ic_attrs, c, 11, XIMTYPE_WINDOW,
                    "preeditCaretCallback");
    AppendAttrEntry(ic_attrs, c, 12, XIMTYPE_WINDOW,
                    "preeditStateNotifyCallback");
    AppendAttrEntry(ic_attrs, c, 13, XIMTYPE_WINDOW, "statusStartCallback");
    AppendAttrEntry(ic_attrs, c, 14, XIMTYPE_WINDOW, "statusDoneCallback");
    AppendAttrEntry(ic_attrs, c, 15, XIMTYPE_WINDOW, "statusDrawCallback");
    Put16(b, c, (uint16_t)ic_attrs.size());
    Put16(b, c, 0); /* IC 段头的跳过字段 (client 不解析) */
    b += ic_attrs;
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
        /* 跳过包尾零填充: client 的 CM 分片恒 20B (末块 _XimXWrite 先 bzero
         * 再 memcpy, imTrX.c), 不足 4 字节整倍数的包在重组缓冲里留零残渣,
         * 下一个包头会被残渣顶住 (words=0 → total<=4 → break, 整条流卡死
         * —— 2026-10-08 实测: OPEN(16B) 后残留 4 零字节, 38 的 property
         * 包永远派发不出去, client 挂死在等 39)。libX11 重组器同款工序
         * (imTransR.c:196-200); XIM major 恒非 0, 非零字节即下一包头。 */
        size_t z = 0;
        while (z < c.rx.size() && c.rx[z] == '\0')
            ++z;
        c.rx.erase(0, z);
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
        /* 不主动推 ENCODING_NEGOTIATION_REPLY: 该 push 建立在错误根因上
         * (曾误判 client 到不了 nego)。实锤 (2026-10-08, imRmAttr.c 逐字节
         * 对出): client 死在 _XimOpen 内的 _XimGetAttributeID —— OPEN_REPLY
         * 缺段长前缀, 属性表解析 n=0 返回 False, 38 从未有机会发出。段长
         * 前缀修复后 client 会自己发 38, case 38 正常应答。 */
        fprintf(stderr, "[xim-server] OPEN im_id=%u locale-first=%d\n",
                c.im_id, pkt.size() > 4 ? (int)p[0] : -1);
        break;
    }
    case XIM_GET_IM_VALUES:
    {
        /* 请求 [im-id:2][id 列表字节长:2][attr-id:2...] —— 第二字段是字节
         * 数不是条目数 (imDefIm.c:1480 buf_s[1]=len 且 len 由 MakeIMAttrIDList
         * 按字节回填)。只认 queryInputStyle (id 0, 我们的 OPEN_REPLY 指定)。
         * REPLY: [im-id:2][属性区字节长:2] + XIMATTRIBUTE[id:2][vlen:2][value]。
         * XIMSTYLES 的 value = [num:2][pad:2][CARD32×num] —— styles 从第 4
         * 字节起 (imRmAttr.c:255 style_list=&data[2] 即字节偏移 4), 校验
         * vlen ≥ 4+num*4 (imRmAttr.c:268): 首版 [num:2][style:4]=6 字节被
         * 判 8>6 False → styles=NULL → wine "Could not find supported input
         * style" → XCloseIM → XRegisterIMInstantiateCallback 立即重查 (我们
         * 的 server 对任意 im_name 都接受) → xim_open→xim_create→XCloseIM
         * 死循环 (2026-10-08 实测单会话 11 次 OPEN), 旋转线程拖住 display
         * lock, wine 全进程初始化爬行 —— input-keyboard exe 卡死的根因。 */
        uint16_t id_bytes = pkt.size() >= 8 ? Get16(c, p + 2) : 0;
        uint16_t n = id_bytes / 2;
        bool only_style = (n == 1 && pkt.size() >= 10 && Get16(c, p + 4) == 0);
        std::string b;
        Put16(b, c, c.im_id);
        std::string attr;
        if (only_style)
        {
            std::string val;
            Put16(val, c, 1); /* 1 组 style */
            Put16(val, c, 0); /* XIMSTYLES 头部的 pad (styles 第 4 字节起) */
            Put32(val, c, 0x0808); /* PreeditNothing | StatusNothing */
            Put16(attr, c, 0); /* queryInputStyle 的 attr id */
            Put16(attr, c, (uint16_t)val.size());
            attr += val;
            attr.append((4 - (val.size() % 4)) % 4, '\0');
        }
        /* REPLY 的 length 字段 = 属性区字节数 (xim.xml: "2 n byte length";
         * client _XimProtoGetIMValues data_len=buf_s[1] 后按 total>=4 字节
         * 走段, imRmAttr.c:469 —— 首版发字数, client total=1<4 解析失败)。 */
        Put16(b, c, (uint16_t)attr.size());
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
        c.ic_id = c.next_ic;
        c.next_ic++;
        fprintf(stderr, "[xim-server] CREATE_IC ic_id=%u\n", c.ic_id);
        break;
    }
    case XIM_SET_IC_VALUES:
    {
        /* 请求 [im-id:2][ic-id:2][XICATTRIBUTE...] — REPLY 按规范带回双 id
         * (xim.xml:1845-1849; 首版漏 ic-id, 靠零填充侥幸过, 显式补上)。 */
        c.ic_id = pkt.size() >= 8 ? Get16(c, p + 2) : c.ic_id;
        fprintf(stderr, "[xim-server] SET_IC_VALUES ic=%u payload=%zu\n",
                c.ic_id, pkt.size());
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
        Put16(b, c, c.ic_id);
        SendPacket(c, XIM_SET_IC_VALUES_REPLY, 0, b);
        break;
    }
    case XIM_GET_IC_VALUES:
    {
        /* 请求 [im-id:2][ic-id:2][n:2][attr-id:2...] — 首版把 ic-id 当 n、
         * 属性 id 前移 2 字节全错位 (xim.xml: GET_IC_VALUES 请求布局)。
         * REPLY [im-id:2][ic-id:2][n 字节:2][unused:2][attrs] (xim.xml:1873;
         * client data=&buf_s[4]/data_len=buf_s[2], imDefIc.c:406-407) ——
         * 首版缺 [ic-id][unused], client 从属性区首 2 字节读 n → 解析必败。 */
        c.ic_id = pkt.size() >= 6 ? Get16(c, p + 2) : c.ic_id;
        uint16_t n = pkt.size() >= 8 ? Get16(c, p + 4) : 0;
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, c.ic_id);
        std::string vals;
        for (uint16_t i = 0; i < n && 8 + (size_t)i * 2 <= pkt.size(); i++)
        {
            uint16_t id = Get16(c, p + 6 + (size_t)i * 2);
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
        Put16(b, c, 0); /* unused (xim.xml:1876) */
        b += vals;
        SendPacket(c, XIM_GET_IC_VALUES_REPLY, 0, b);
        fprintf(stderr, "[xim-server] GET_IC_VALUES n=%u\n", n);
        break;
    }
    case XIM_ENCODING_NEGOTIATION:
    {
        /* REPLY [im-id:2][category:2][idx:2] — idx = XIM_Default_Encoding_IDX
         * (0xFFFF, -1 截断): libX11 1.8.10 _XimGetEncoding 只认 COMPOUND_TEXT
         * (name 列表无其它可用分支), Default 走同一组 ct 转换器。category 取
         * DetailCategory(1) — NameCategory 会再遍历 client 的 name 列表
         * (_XimGetEncoding Default 分支后不 return), detail 分支 "Not yet"
         * 为空, 转换器完全由 Default idx 决定。不回 REPLY 客户端会卡死在
         * XOpenIM 的 negotiation 往返 (_XimRead 阻塞等)。 */
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, 1); /* XIM_Encoding_DetailCategory (XimProto.h:172) */
        Put16(b, c, 0xFFFF);
        SendPacket(c, XIM_ENCODING_NEGOTIATION_REPLY, 0, b);
        fprintf(stderr, "[xim-server] ENCODING_NEGOTIATION -> default CT\n");
        break;
    }
    case XIM_DESTROY_IC:
    {
        /* client 的 XDestroyIC 会等 REPLY (imDefIc.c _XimProtoDestroyIC
         * 阻塞读), 不回则探针/应用挂在销毁 IC 上。请求与应答同为
         * [im-id:2][ic-id:2] (xim.xml)。 */
        uint16_t ic = pkt.size() >= 6 ? Get16(c, p + 2) : c.ic_id;
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, ic);
        SendPacket(c, XIM_DESTROY_IC_REPLY, 0, b);
        if (c.ic_focus && (ic == c.ic_id || c.ic_id == 0))
            c.ic_focus = false;
        fprintf(stderr, "[xim-server] DESTROY_IC ic=%u\n", ic);
        break;
    }
    case XIM_SET_IC_FOCUS:
        /* 异步无应答 (xim.xml)。client 的 XSetICFocus 在窗口收键盘焦点时
         * 调 (wine xim.c xim_set_focus) —— commit 目标定位的①优先级依据 */
        c.ic_id = pkt.size() >= 6 ? Get16(c, p + 2) : c.ic_id;
        for (XimClient &o : g_xims)
            o.ic_focus = false;
        c.ic_focus = true;
        fprintf(stderr, "[xim-server] SET_IC_FOCUS im=%u ic=%u win=0x%lx\n",
                c.im_id, c.ic_id, (unsigned long)c.ic_client_win);
        break;
    case XIM_QUERY_EXTENSION:
    {
        /* client 在 OPEN 属性解析完、nego 之前必发 (imDefIm.c:900 链,
         * libX11 自带 XIM_EXT_SET_EVENT_MASK 等扩展名清单)。REPLY
         * [im-id:2][n:2 字节长][LISTofEXT] — 回空清单 (n=0): client 的
         * _XimParseExtensionList 对 0 直接 True (imExten.c:367-368),
         * 扩展特性 (EXT_SET_EVENT_MASK 等) 全部禁用, 主线不受影响。
         * 不回则 client 挂死在 XOpenIM 内等 41 (2026-10-08 实测:
         * "unhandled major=40" 后探针停摆)。 */
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, 0);
        SendPacket(c, XIM_QUERY_EXTENSION_REPLY, 0, b);
        fprintf(stderr, "[xim-server] QUERY_EXTENSION -> empty list\n");
        break;
    }
    case XIM_CLOSE:
    {
        /* XCloseIM 发 CLOSE 等 REPLY (imDefIm.c _XimCloseCheck 阻塞读);
         * 请求 [im-id:2]、应答同布局 (xim.xml:1410)。 */
        std::string b;
        Put16(b, c, c.im_id);
        SendPacket(c, XIM_CLOSE_REPLY, 0, b);
        fprintf(stderr, "[xim-server] CLOSE im=%u\n", c.im_id);
        break;
    }
    case XIM_SYNC:
    {
        /* 请求/应答同布局 [im-id:2][ic-id:2] (xim.xml) — 回显请求的 ic-id */
        uint16_t ic = pkt.size() >= 6 ? Get16(c, p + 2) : 0;
        std::string b;
        Put16(b, c, c.im_id);
        Put16(b, c, ic);
        SendPacket(c, XIM_SYNC_REPLY, 0, b);
        break;
    }
    case XIM_DISCONNECT:
        SendPacket(c, XIM_DISCONNECT_REPLY, 0, std::string());
        c.active = false;
        fprintf(stderr, "[xim-server] DISCONNECT\n");
        break;
    default:
    {
        fprintf(stderr, "[xim-server] unhandled major=%u pkt=%zu hex:",
                major, pkt.size());
        for (size_t i = 0; i < pkt.size() && i < 24; ++i)
            fprintf(stderr, " %02x", (unsigned char)pkt[i]);
        fprintf(stderr, "\n");
        break;
    }
    }
}

/* ── Task 4: 通道 → XIM_COMMIT ───────────────────────────────────────
 * Xlib 单线程纪律: SendPacket/XGetInputFocus 只许主循环线程动 g_dpy。
 * chan 线程只落槽 (覆盖式 —— 未发的旧串被新串顶替, 输入法场景丢旧是
 * 正确语义), 主循环 5ms 粒度取走发送。 */
static pthread_mutex_t g_commit_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::string g_commit_pending;

/* XIM_COMMIT 载荷 (libX11 imDefLkup.c:_XimCommitRecv 实读):
 * [im-id:2][ic-id:2][flag:2][len:2][bytes][pad4], flag=XimLookupChars
 * (0x0002, XimProto.h:183; 无 sync 位 → client 不回包)。
 * 串编码 = UTF-8: wine 消费走 XmbLookupString, 进程 locale
 * zh_CN.UTF-8 (Task 3 LOCALES 应答) → mb 直通 UTF-8。 */
static void DrainPendingCommit(void)
{
    std::string text;
    pthread_mutex_lock(&g_commit_mutex);
    if (g_commit_pending.empty())
    {
        pthread_mutex_unlock(&g_commit_mutex);
        return;
    }
    text.swap(g_commit_pending);
    pthread_mutex_unlock(&g_commit_mutex);

    /* commit 目标定位 (按优先级): ①ic_focus 标记的 client (XIM_SET_IC_FOCUS
     * 实时维护) ②XGetInputFocus 焦点窗 == 该 client 的 ic_client_win
     * ③唯一「有 IC 的 active」client。g_xims 里存着历史会话/未建 IC 的
     * 连接 (explorer 只 OPEN 不 CREATE_IC), 首版兜底「取最后一个 active」
     * 会选中它们 → "commit dropped: client has no IC" (2026-10-08 实测)。 */
    Window focus = None;
    int revert = 0;
    XGetInputFocus(g_dpy, &focus, &revert);
    XimClient *target = NULL;
    for (XimClient &c : g_xims)
        if (c.active && c.ic_focus)
            target = &c;
    if (!target)
        for (XimClient &c : g_xims)
            if (c.active && c.ic_id && c.ic_client_win != None &&
                c.ic_client_win == focus)
                target = &c;
    if (!target)
    {
        XimClient *last = NULL;
        for (XimClient &c : g_xims)
            if (c.active && c.ic_id)
                last = &c;
        /* 唯一有 IC 的 client 才兜底; 多个时无法裁决, 报明细 */
        target = last;
        if (target)
            for (XimClient &c : g_xims)
                if (c.active && c.ic_id && &c != target)
                    target = NULL;
    }
    if (!target)
    {
        fprintf(stderr, "[xim-server] commit dropped: no focused/IC client "
                        "(focus=0x%lx)\n", (unsigned long)focus);
        return;
    }
    /* UTF-8 → COMPOUND_TEXT: libX11 1.8.10 的 _XimGetEncoding 只认
     * COMPOUND_TEXT (name 列表无 UTF-8 分支, Default idx 同样建 ct 转换器),
     * COMMIT 串按 CT 编码; wine 端 XmbLookupString 走 ctom (CT→mb),
     * locale zh_CN.UTF-8 下还原成 UTF-8。纯 ASCII 时 Xutf8→CT 恒等。 */
    char *list[1] = {(char *)text.c_str()};
    XTextProperty tp = {};
    if (Xutf8TextListToTextProperty(g_dpy, list, 1, XCompoundTextStyle, &tp) ==
            Success && tp.value && tp.nitems > 0)
    {
        text.assign((const char *)tp.value, tp.nitems);
        XFree(tp.value);
    }
    else
    {
        fprintf(stderr, "[xim-server] UTF8->CT failed (locale?), raw utf8\n");
    }
    uint16_t ic = target->ic_id;
    {
        /* CT 载荷留证: client 解码失败时先看 server 究竟发了什么字节 */
        std::string hex;
        char tmp[8];
        for (size_t i = 0; i < text.size() && i < 16; ++i)
        {
            snprintf(tmp, sizeof(tmp), "%02x ", (unsigned char)text[i]);
            hex += tmp;
        }
        fprintf(stderr, "[xim-server] commit payload (%zu): %s\n",
                text.size(), hex.c_str());
    }
    std::string b;
    Put16(b, *target, target->im_id);
    Put16(b, *target, ic);
    Put16(b, *target, 0x0002);
    Put16(b, *target, (uint16_t)text.size());
    b += text;
    b.append((4 - (text.size() % 4)) % 4, '\0');
    SendPacket(*target, XIM_COMMIT, 0, b);
    fprintf(stderr, "[xim-server] COMMIT len=%zu im=%u ic=%u focus=0x%lx\n",
            text.size(), target->im_id, ic, (unsigned long)focus);
}

/* socket 读线程: 主进程转投的 commit 请求 → 落槽 (Task 4)。 */
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
        pthread_mutex_lock(&g_commit_mutex);
        bool overwrite = !g_commit_pending.empty();
        g_commit_pending = text;
        pthread_mutex_unlock(&g_commit_mutex);
        fprintf(stderr, "[xim-server] chan text len=%u queued%s\n", hdr[1],
                overwrite ? " (overtook unsent)" : "");
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

    /* Task 4: COMMIT 串的 UTF-8→CT 转换 (Xutf8TextListToTextProperty) 依赖
     * locale: XLOCALEDIR 指到打包的 X11 locale 目录 (与 wine 子进程同源,
     * wine_env_baseline.h 同款路径), locale 显式 zh_CN.UTF-8 (Task 3 的
     * LOCALES 应答同值; NCP env 无 LANG, setlocale("") 会落 C)。 */
    if (!parts[1].empty())
    {
        std::string localeDir = parts[1] + "/../wine/share/X11/locale";
        setenv("XLOCALEDIR", localeDir.c_str(), 1);
    }
    setlocale(LC_CTYPE, "zh_CN.UTF-8");

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

    /* 异步 X 错误不许杀进程: client 退出瞬间其窗口销毁, 对死窗口的
     * XSendEvent (major 25) 会产生 BadWindow——Xlib 默认错误处理器打印
     * 后直接 exit 本进程 (2026-10-08 实测: 探针退出竞态把 xim server
     * 带死, D32 链第二现场)。server 是长生命周期服务, 单个 client 的
     * 竞态错误只应丢弃该次交互。 */
    XSetErrorHandler([](Display *, XErrorEvent *ev) -> int {
        fprintf(stderr, "[xim-server] X error opcode=%u id=0x%lx (ignored)\n",
                (unsigned)ev->request_code, (unsigned long)ev->resourceid);
        return 0;
    });

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
                         * delete=True 读走 (Transport Table 4-7)。property
                         * 写在 srv_win —— libX11 imTrX.c:288 实读:
                         * XChangeProperty(dpy, spec->ims_connect_wid, ...)
                         * (ims_connect_wid = XCONNECT 应答里给的 c->srv_win);
                         * 首版读 xconnect_win → 大包永远收不到 → ENCODING_
                         * NEGOTIATION 无应答, wine XOpenIM 卡死, IC 从未
                         * 创建 (2026-10-08 实测 char-count 断在此)。 */
                        Atom prop = (Atom)ev.xclient.data.l[1];
                        unsigned long n = 0, left = 0;
                        Atom type = None;
                        int fmt = 0;
                        unsigned char *data = NULL;
                        if (XGetWindowProperty(g_dpy, c->srv_win, prop,
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
        DrainPendingCommit(); /* 通道落槽的 commit (Task 4), 主线程发送 */
    }
}
