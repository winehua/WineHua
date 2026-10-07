# XIM server 实施计划（x11 路线 IME/Unicode 通道）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** x11 路线上建最小 XIM server，打通 Unicode 串 → winex11 WM_CHAR（smoke ime 动作转绿 + 产品面 IME 打字底座）。

**Architecture:** 合成器进程内新增 `display/xim_bridge.c`，以 X client 身份连接本机 Xwayland，实现最小 XIM 协议 server（发现/握手/IC/COMMIT）；smoke 面经既有注入队列接入，产品面经 wlroots text-input-v3 接入。wine 侧零改动（xim.c 走标准 libX11 `XOpenIM`，协议对端是 libX11 ximcp）。

**Tech Stack:** C（Xlib + XIM 协议 wire 格式）、wlroots text-input-v3、既有 display 注入队列（pthread 队列 + loop 线程 drain 纪律）。

**Spec:** `docs/superpowers/specs/2026-10-07-display-route-x11-xim-server-design.md`（本计划从它论证，两者随行；执行者先读 spec 再读本计划）。

## Global Constraints

- wine 侧零改动：`thirdparty/wine/dlls/winex11.drv/xim.c` 保持上游原版。
- X 键盘链（keymap/物理键注入）不动；keymap 扩展否证结论不复活（spec §2）。
- 虚拟桌面零回退：每 Task 收口跑 `python3 automation/smoke.py run --job smoke/jobs/displayroute-win32-interactive.json --display-route x11 --tests input-wheel-x64,input-mouse-x64,e2e-click-x64` 全 PASS。
- 构建纪律：`set -o pipefail`；改 `entry/src/main/cpp/` 后 `rm -f entry/build/default/intermediates/libs/default/arm64-v8a/libentry.so` 强制重建再 `make NATIVE_ARCH=arm64-v8a hap`；部署 `bash scripts/package.sh deploy 192.168.0.206:33363`。
- 产品不为测试让步：smoke 接入走 `smoke/` 资产，产品路径不引用测试程序。
- 设备端只跑不判；判定只读归档。

## Review Focus

1. **XIM locale 组合**（wine 请求 `zh_CN`/`C` locale 的 OPEN）——桥必须对两者回有效 OPEN_REPLY，否则 XOpenIM 静默失败。Task 3 步骤 4 的双 locale 判据钉住。
2. **多 wine 进程并发**（explorer + 应用进程各自 xim_thread_attach）——桥必须接受多 client 连接，单连接状态机不能串台。Task 3 步骤 5 钉住。
3. **UTF-8 vs compound text**（COMMIT 编码协商）——桥声明 UTF-8 且串为 UTF-8，wine 侧 `XmbResetIC`/commit 链按 locale 折 WM_CHAR；编码错则 char-cjk 收到乱码。Task 4 步骤 3 的 `chars` metric 逐字节核对钉住。
4. **线程边界**（Xlib 连接跨线程使用）——桥的 X 连接必须单线程独占（loop 线程），跨线程请求经既有注入队列转交；违者 Xlib 断言崩溃。Task 2 步骤 1 的结构即钉住。
5. **探针自缓存残留**（X1 标记文件跨部署残留导致探针不再跑）——标记文件名带构建无法感知的部署代际时用「每次链启动都跑、仅日志不落盘」替代缓存。Task 1 步骤 1 结构即钉住。

---

### Task 1: X1 探针——app 主进程 X 连接与 property 写入可行性

**Files:**
- Create: `entry/src/main/cpp/display/xim_bridge.c`（骨架：仅探针函数）
- Create: `entry/src/main/cpp/display/xim_bridge.h`
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（链启动处调用探针，~5 行）
- Modify: `entry/src/main/cpp/display/BUILD.gn` 或对应 cmake 源清单（加 xim_bridge.c；跟随 display_input.c 的现有编译目标）

**Interfaces:**
- Produces: `int xim_bridge_probe_start(void)`——链启动后调用一次，返回 0=探针已发起（异步结果走 hilog）；内部自管 X 连接生命周期。
- Produces: 后续 Task 的 `struct xim_bridge *` 生态在此文件内长出。

- [ ] **Step 1: 写探针（真代码）**

`xim_bridge.c` 核心内容：

```c
/* xim_bridge.c — x11 路线 IME/Unicode 通道 (XIM server, spec 2026-10-07-xim)。
 * 线程纪律: 本文件的 Xlib 连接 (g_xdpy) 仅在合成器 loop 线程使用;
 * 跨线程请求经 display_input 注入队列转交 (同 D19 剪贴板桥纪律)。
 *
 * X1 探针 (本 Task): 验证 (a) app 主进程 XOpenDisplay(":0") 可行
 * (b) root window _XIM_SERVERS property 写入成功。每次链启动都跑、
 * 结果只进 hilog (不做标记文件缓存——部署代际无法感知, 见 Review Focus 5)。 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <hilog/log.h>
#define TAG "xim-bridge"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

static Display *g_xdpy;

int xim_bridge_probe_start(void)
{
    g_xdpy = XOpenDisplay(":0");
    if (!g_xdpy) {
        LOGE("X1 probe: XOpenDisplay(:0) FAILED (sandbox 权限面?)");
        return -1;
    }
    Window root = DefaultRootWindow(g_xdpy);
    Atom servers = XInternAtom(g_xdpy, "XIM_SERVERS", False);
    const char *name = "winehua";
    XChangeProperty(g_xdpy, root, servers, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)&servers, 1);
    /* 回读核对 (判定不靠写入返回值——X server 不回错, 必须回读) */
    Atom type; int fmt; unsigned long n, left; unsigned char *data = NULL;
    int ok = 0;
    if (XGetWindowProperty(g_xdpy, root, servers, 0, 64, False, XA_ATOM,
                           &type, &fmt, &n, &left, &data) == Success && data) {
        ok = (n >= 1 && type == XA_ATOM);
        XFree(data);
    }
    LOGI("X1 probe: display=%{public}s root_property_write=%{public}s "
         "vendor=%{public}s",
         DisplayString(g_xdpy), ok ? "OK" : "FAIL",
         XServerVendor(g_xdpy));
    /* 连接保持: Task 2 起此连接成为 XIM server 主连接 (不关)。 */
    return 0;
}
```

`xim_bridge.h`：

```c
#ifndef WINEHUA_XIM_BRIDGE_H
#define WINEHUA_XIM_BRIDGE_H
/* x11 路线 XIM server 桥 (spec 2026-10-07-xim)。链启动后调用;
 * 结果与状态走 hilog (tag: xim-bridge)。 */
int xim_bridge_probe_start(void);
#endif
```

display_compositor.cpp 挂点：`wl_ohos_output_chain_start` 成功返回后（现
`evt:desktop-ready` 补票路径附近）加 `xim_bridge_probe_start();` 一行 + include。

- [ ] **Step 2: 构建 + 部署 + 看判据**

```bash
set -o pipefail
rm -f entry/build/default/intermediates/libs/default/arm64-v8a/libentry.so
make NATIVE_ARCH=arm64-v8a hap 2>&1 | tail -3
bash scripts/package.sh deploy 192.168.0.206:33363 2>&1 | tail -1
hdc -t 192.168.0.206:33363 shell "aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.displayRoute x11"
sleep 20
hdc -t 192.168.0.206:33363 shell "hilog -x 2>/dev/null | grep 'X1 probe' | tail -2"
```

Expected: `X1 probe: display=:0.0 root_property_write=OK vendor=...`。
若 `XOpenDisplay FAILED` → **停止**，spec §3 架构改 NCP 子进程承载（回 spec review），不要原地绕。

- [ ] **Step 3: 虚拟桌面零回退门**

```bash
python3 automation/smoke.py run --job smoke/jobs/displayroute-win32-interactive.json --display-route x11 --tests input-wheel-x64,input-mouse-x64,e2e-click-x64 2>&1 | tail -3
```

Expected: 3/3 PASS。

- [ ] **Step 4: Commit**

```bash
git add entry/src/main/cpp/display/xim_bridge.c entry/src/main/cpp/display/xim_bridge.h entry/src/main/cpp/display/display_compositor.cpp
git commit -m "feat(display): X1 探针——主进程 X 连接与 XIM_SERVERS property 写入 (XIM spec §6 X1)"
```

---

### Task 2: XIM server 注册与 selection 应答（libX11 发现面）

**Files:**
- Modify: `entry/src/main/cpp/display/xim_bridge.c`（注册 + selection owner + 事件循环接入）

**Interfaces:**
- Consumes: Task 1 的 `g_xdpy`（loop 线程独占）。
- Produces: `void xim_bridge_tick(void)`——桥事件泵，由既有帧时钟每拍调（ohos_output.c FrameTick 内加一行；XPending 处理，无事件立即返回，零开销）。

- [ ] **Step 1: 注册面实现（真代码骨架）**

在 xim_bridge.c 追加（Task 1 的探针函数改造为 `xim_bridge_start`）：

```c
/* XIM server 发现面 (libX11 ximcp 的探测序列, 锚定 Xlib 内置 IM 实现):
 * 1. root 的 XIM_SERVERS property (ATOM 类型) 列出 server 名 atom
 * 2. selection XIM_SERVERS 的 owner = 本桥窗口
 * 3. libX11 XOpenIM 读 XMODIFIERS=@im=winehua → 匹配 property →
 *    XConvertSelection(XIM_SERVERS) → SelectionNotify → 向 owner 窗口
 *    发 _XIM_MOREDATA/_XIM_PROTOCOL ClientMessage 握手 (Task 3) */
static Window g_srv_win;

static char *SelTargets(Display *d, Window w); /* selection 应答体, 见下 */

void xim_bridge_tick(void)
{
    if (!g_xdpy) return;
    while (XPending(g_xdpy)) {
        XEvent ev;
        XNextEvent(g_xdpy, &ev);
        if (ev.type == SelectionRequest) {
            XSelectionEvent sev = {
                .type = SelectionNotify, .display = g_xdpy,
                .requestor = ev.xselectionrequest.requestor,
                .selection = ev.xselectionrequest.selection,
                .target = ev.xselectionrequest.target,
                .property = ev.xselectionrequest.property, .time = CurrentTime };
            const char *names = "winehua\0";
            XChangeProperty(g_xdpy, sev.requestor, sev.property, XA_ATOM, 32,
                            PropModeReplace,
                            (const unsigned char *)&(Atom){XInternAtom(g_xdpy, "winehua", False)},
                            1);
            XSendEvent(g_xdpy, sev.requestor, False, 0, (XEvent *)&sev);
        }
    }
}
```

xim_bridge_start 追加：`XSelectInput` 无需（SelectionRequest 要选——
`XSelectInput(g_xdpy, root, 0)` 不够；SelectionRequest 送达 owner 窗口，
`g_srv_win = XCreateSimpleWindow(...)` 后 `XSetSelectionOwner(g_xdpy,
XInternAtom(g_xdpy,"XIM_SERVERS",False), g_srv_win, CurrentTime)`，
`XSelectInput(g_xdpy, g_srv_win, 0)`（SelectionRequest 事件无需窗口选掩码，
Xlib 全收）。

ohos_output.c FrameTick 内加 `xim_bridge_tick();`（include xim_bridge.h）。

- [ ] **Step 2: 构建部署 + 判据**

判据（新增桥日志，start 时打）：`xim-bridge: registered selection XIM_SERVERS owner=0x...`。
wine 侧判据（发现面被触发）：

```bash
# 套件 env 加 WINEHUA_WINEDEBUG=+xim 临时跑 input-keyboard-x64:
python3 automation/smoke.py run --job smoke/jobs/displayroute-win32-interactive.json \
  --display-route x11 --tests input-keyboard-x64 --env WINEHUA_WINEDEBUG=+xim 2>&1 | tail -3
hdc -t 192.168.0.206:33363 file recv -b app.hackeris.winehua /data/storage/el2/base/temp/wine_stderr_$(date +%Y%m%d).log /home/rianm/.claude/jobs/5a90fd84/tmp/wsx.log
grep -c "trace:xim" /home/rianm/.claude/jobs/5a90fd84/tmp/wsx.log
```

Expected: `trace:xim` 行数 > 0（XOpenIM 被尝试；此时握手未完成属预期——Task 3 补）。若为 0：查 XMODIFIERS 未下发（Task 6 的 env 项提前到本 Task 验证——wine_env.cpp:104 附近加 `env.push_back("XMODIFIERS=@im=winehua");` 并重跑本步）。

- [ ] **Step 3: 零回退门（同 Task 1 Step 3，3/3 PASS）**

- [ ] **Step 4: Commit**

```bash
git add entry/src/main/cpp/display/xim_bridge.c entry/src/main/cpp/display/ohos_output.c entry/src/main/cpp/wine/wine_env.cpp
git commit -m "feat(display): XIM server 注册面 + 帧时钟事件泵 + XMODIFIERS 下发 (XIM spec Task 2)"
```

---

### Task 3: XIM 协议握手（XCONNECT → CONNECT → OPEN → OPEN_REPLY）

**Files:**
- Modify: `entry/src/main/cpp/display/xim_bridge.c`（ClientMessage 状态机）

**Interfaces:**
- Consumes: Task 2 的 selection 注册（libX11 拿到 owner 后向 `g_srv_win` 发 ClientMessage）。
- Produces: `struct xim_client`（每 wine 进程一个：Window xconnect_win、 major/minor 版本、locale 名、byte order）；多 client 表（数组 ≤8，覆盖 explorer+应用并发，见 Review Focus 2）。

- [ ] **Step 1: 协议常量与状态机骨架（真代码）**

XIM wire 协议要点（对端 libX11 ximcp，锚定 XIM 协议规范「The Input Method Protocol」；包格式 = ClientMessage 32 字节 + 超长数据走 `XIM_PROTOCOL` property）：

```c
/* 事件类型 (XIM wire): */
#define XIM_CONNECT 1
#define XIM_CONNECT_REPLY 2
#define XIM_OPEN 30
#define XIM_OPEN_REPLY 31
#define XIM_CLOSE 32
#define XIM_CREATE_IC 20
#define XIM_COMMIT 69   /* XIM_COMMIT = 69 (XIM_LOOKUPCHARS 子模式) */
#define XIM_STR_CONVERSION 73
/* transport: libX11 发 ClientMessage (message_type=_XIM_PROTOCOL),
 * l[0]=ser#, l[1]=major opcode, l[2]=minor, 载荷走 _XIM_PROTOCOL property
 * (连接建立后), 或 l[3..] 内联 (小包)。回程同构, 目标 = client 的
 * xconnect window。 */
```

状态机：`ClientMessage(message_type=XInternAtom("_XIM_PROTOCOL"))` →
按 major opcode 分派：
- `XIM_CONNECT`: 读 byte-order('l'/'B') + 版本 → 建表项 → 回 `XIM_CONNECT_REPLY`(接受)
- `XIM_OPEN`: 读 locale 名（UTF-8 外部名，"zh_CN"/"C" 都要收，Review Focus 1）→ 回 `XIM_OPEN_REPLY`：**最小 IM 属性表**（separator: XIM_NS Native-Style ";\x00"、input-styles 至少含 `OffTheSpot,None` 组合对 `XIMSTYLEDRAW` 之外 wine 只用 on-spot——styles 列表回 `[(XIMPreeditNone|XIMStatusNone), (XIMPreeditPosition|XIMStatusArea), (XIMPreeditNothing|XIMStatusNothing)]` 三组，锚定 wine xim.c 的 `xim_create` 对 style 的过滤（xim.c:440-455 一带, 实读为准））
- `XIM_CREATE_IC`: 回 IC id（递增）——IC 属性读写最小实现 = 全部忽略写、GET_IC_VALUES 回过滤属性（Task 4 再按 wine 实际 GET 列表补）

超长载荷：事件 l[2] 后带 property 名时读 `_XIM_PROTOCOL` property 完整包（XIM 包头 = 2 字节 ser + 2 字节 major + 2 字节 minor + 2 字节 length（32bit words）+ 载荷），组包/拆包写两个 helper（`xim_pkt_put`/`xim_pkt_get`，大端序统一——XIM wire 固定大端，CONNECT 阶段协商的 byte order 只影响后续整数字段——锚定 libX11 实测行为，按实测日志修）。

- [ ] **Step 2: 构建部署 + 逐跳判据**

```bash
# 同 Task 2 Step 2 的 +xim 跑批, 收 wine stderr 后:
grep -E "trace:xim" /home/rianm/.claude/jobs/5a90fd84/tmp/wsx.log | tail -10
```

Expected 顺序（wine 侧）：`xim_create`/`XOpenIM` 成功路径日志 + 无
`Failed to open input method`。桥侧日志：`xim-bridge: client connected win=0x... locale=zh_CN`。
判据不达时的调参纪律：**按桥日志最后一跳打**（停滞类问题：最后一条日志就是最后一跳，build-and-log 规则 8）——XIM 握手逐包对账以 libX11 实测行为为准，协议文档存疑处一律以「wine 挂载成功」为验收。

- [ ] **Step 3: 零回退门（3/3 PASS）**

- [ ] **Step 4: Commit**

```bash
git add entry/src/main/cpp/display/xim_bridge.c
git commit -m "feat(display): XIM 握手状态机——CONNECT/OPEN/OPEN_REPLY 多 client (XIM spec Task 3)"
```

---

### Task 4: IC 管理 + COMMIT + smoke ime 接入

**Files:**
- Modify: `entry/src/main/cpp/display/xim_bridge.c`（IC 表 + commit API）
- Modify: `entry/src/main/cpp/display/display_input.c`（队列 is_text 分支恢复 + drain 调 commit）
- Modify: `entry/src/main/cpp/display/display_input.h`（`wl_ohos_input_post_text` 声明）
- Modify: `entry/src/main/cpp/display_route/display_route_napi.cpp`（`injectDisplayRouteText` NAPI 恢复）
- Modify: `entry/src/main/cpp/types/display_route_napi/Index.d.ts`（声明）
- Modify: `entry/src/main/ets/smoke/SmokeRunner.ets`（ime 动作 x11 分支）

**Interfaces:**
- Consumes: Task 3 的 client 表（commit 目标 = 键盘焦点窗所属 client）。
- Produces: `void xim_bridge_commit_text(const char *utf8)`——loop 线程调用，向当前焦点 IC 所在 client 发 XIM_COMMIT(XIM_LOOKUPCHARS, UTF-8 串)。
- Produces: `void wl_ohos_input_post_text(const char *utf8)`——任意线程安全入口（队列 is_text item，drain 在 loop 线程调 commit）。

- [ ] **Step 1: IC 表与焦点跟随（真代码骨架）**

```c
/* IC 最小语义: wine 对每个带焦点窗口建 XIC (xim.c xic_create); 桥按
 * client 记 IC 列表 (ic id -> focus window)。commit 目标 = X 侧当前
 * 输入焦点窗口 (XGetInputFocus) 所属 IC; 查不到时取该 client 最近 IC
 * (wine 单窗应用只有一个 IC)。 */
struct xim_client { Window xconnect; char locale[32]; int n_ic; int last_ic; };
```

`xim_bridge_commit_text`：组 `XIM_COMMIT` 包（minor = XIM_LOOKUPCHARS=1，
载荷 = flag(1B XimCommitWithKeyPrint=0) + keyprint len + 串），经 client 的
xconnect window 以 ClientMessage(_XIM_PROTOCOL)+超长 property 发送。

- [ ] **Step 2: smoke 通道接线（恢复已撤实验的结构, 机制换 XIM）**

display_input.c：`struct inject_item` 加 `bool is_text;`（keycode 字段存
UCS——**本 Task 仅整串透传**，`char *text` 字段 strdup 整串，drain 调
`xim_bridge_commit_text(item.text)` 后 free）；`wl_ohos_input_post_text`
（UTF-8 串入队）。display_route_napi.cpp / Index.d.ts / SmokeRunner ime 分支
x11 调 `displayRouteNapi.injectDisplayRouteText(text)`——三处与已撤实验
diff 相同（git show 0009924 前一版的撤除面可参考，重新实现按本步语义）。

- [ ] **Step 3: 判据——keyboard 单项转绿**

```bash
set -o pipefail
rm -f entry/build/default/intermediates/libs/default/arm64-v8a/libentry.so
make NATIVE_ARCH=arm64-v8a hap 2>&1 | tail -2
bash scripts/package.sh deploy 192.168.0.206:33363 2>&1 | tail -1
python3 automation/smoke.py run --job smoke/jobs/displayroute-win32-interactive.json \
  --display-route x11 --tests input-keyboard-x64,input-keyboard-x86 2>&1 | tail -4
```

Expected: 2/2 PASS，`chars` metric = `0041,0061,0031,4E2D`。
若 char-cjk 收到乱码：Review Focus 3（编码协商）——检查桥 COMMIT 的编码声明
与 wine locale 折算，按 `chars` metric 实际字节定位。

- [ ] **Step 4: 零回退门（3/3 PASS）**

- [ ] **Step 5: Commit**

```bash
git add entry/src/main/cpp/display/xim_bridge.c entry/src/main/cpp/display/display_input.c entry/src/main/cpp/display/display_input.h entry/src/main/cpp/display_route/display_route_napi.cpp entry/src/main/cpp/types/display_route_napi/Index.d.ts entry/src/main/ets/smoke/SmokeRunner.ets
git commit -m "feat(display)+feat(smoke): XIM COMMIT 通道 + ime 动作 x11 接入 (XIM spec Task 4)"
```

---

### Task 5: X2 收口——13 前缀全绿

**Files:**
- Modify: 无新改动（纯验证 + 可能的微修）

- [ ] **Step 1: 13 前缀全量**

```bash
python3 automation/smoke.py run --job smoke/jobs/displayroute-win32-interactive.json --display-route x11 --tests e2e-click-x64,e2e-click-x86,e2e-drag-x64,e2e-drag-x86,e2e-menu-x64,e2e-menu-x86,e2e-resize-x64,e2e-resize-x86,input-mouse-x64,input-mouse-x86,input-keyboard-x64,input-keyboard-x86,input-wheel-x64 2>&1 | tail -4
```

Expected: **13/13 PASS**（X2 达成判据，spec §6）。

- [ ] **Step 2: Commit（如有微修）+ 汇报**

---

### Task 6: X3 产品面——text-input-v3 接入（独立后续，本计划不含实现细节）

X3 依赖 X2 通过与真机 IME 框架联调，作为独立计划再细化（spec §6 已定向：
wlroots `wlr_text_input_v3` enable + commit 监听 → `xim_bridge_commit_text`；
XMODIFIERS 已在 Task 2 落地）。**本计划交付到 X2 为止。**

## Self-Review 记录

1. **Spec coverage**：spec §3 架构（桥落点/X 连接/text-input）→ Task 1-4/6；§4 数据流 → Task 4；§5 风险（XOpenDisplay 可行性→Task 1 停止条件；locale→Review Focus 1+Task 3；多实例→Review Focus 2；text-input 不影响 wayland→display 栈独占）；§6 X1→Task 1、X2→Task 2-5、X3→Task 6 定向；§7 不做清单→未实现 preedit。无缺口。
2. **Placeholder 扫描**：协议常量/状态机/IC 结构均为真代码骨架；「锚定 xim.c 实读」「锚定 libX11 实测行为」为仓库内可读引用，非 TBD。
3. **类型一致性**：`xim_bridge_probe_start`(T1) → Task 2 改造为 start（结构演化在文件内，接口产出以 T2 tick 为准）；`xim_bridge_commit_text`(T4 Produces) 与 display_input drain 调用同名 ✓；`wl_ohos_input_post_text` 签名与已撤实验一致（Index.d.ts 同名恢复）✓。
4. **Review Focus 覆盖**：locale→T3 步骤 2 双 locale 判据；多进程并发→T3 结构（client 表 ≤8）+ 判据含 explorer 场景；编码→T4 步骤 3 逐字节核对；线程边界→T1/T2 结构（loop 独占 + 队列转交）；探针缓存→T1 结构（每次启动都跑）。
