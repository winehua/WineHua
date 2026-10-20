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

### Task 2（NCP 版）: xim_server_child 骨架——spawn + socketpair 通道 + XIM 注册面

**Files:**
- Create: `entry/src/main/cpp/display/ncp/xim_server_child.cpp`（NCP Main 入口）
- Modify: `entry/src/main/cpp/display/xim_bridge.c`（改造：主进程侧通道客户端——socketpair 主端创建/监听 + `xim_bridge_send_text()`；探针函数删除，X1 结论已在案）
- Modify: `entry/src/main/cpp/display/xim_bridge.h`（新接口声明）
- Modify: `entry/src/main/cpp/display/display_compositor.cpp`（`TrySpawnXimServer()`：锚定 `TrySpawnXclientTestClient`（display_compositor.cpp:161-184）与 `g_xwayland_ready_listener`；named fd 传递锚定 xwayland spawn 段的 `wlr_ohos_spawn_xwayland` 接线）
- Modify: `entry/src/main/cpp/CMakeLists.txt`（`add_library(xim_server_child SHARED display/ncp/xim_server_child.cpp)` + 链接同 xclient_child 段：libX11.so.6/libXext.so.6/libhilog/libchild_process + `${WLR_XCB_INC}` include）
- Modify: `entry/src/main/cpp/wine/wine_env.cpp:104` 附近（`env.push_back("XMODIFIERS=@im=winehua");`，与 LANG/LC_ALL 同列）

**Interfaces:**
- Consumes: xclient_child 的 NCP Main 形态（`extern "C" void Main(NativeChildProcess_Args)`，entryParams `|` 分隔，xim_server_child 用 `"<stderrPath>|<xdgDir>"` 两段）；xwayland_child 的命名 fd 传递先例。
- Produces: `int xim_bridge_send_text(const char *utf8)`——任意线程安全（socket write），子进程未就绪返回 -1。
- Produces: 子进程 X 连接面（XOpenDisplay 在 NCP namespace 已证可行，xclient_child.cpp:143-153 + `sockets.c 标准路径优先` 补丁）。

- [ ] **Step 1: 子进程骨架（真代码结构）**

`xim_server_child.cpp` 结构（对齐 xclient_child.cpp:104-141 的 Main/entryParams/stderr 重定向/XDG_RUNTIME_DIR setenv；X 连接后）：

```cpp
// 1. XOpenDisplay(":0") (同 xclient_child.cpp:153)
// 2. XIM 注册面: root 的 XIM_SERVERS property (值 = atom "winehua")
//    + 创建 server 窗口 + XSetSelectionOwner(XIM_SERVERS selection)
// 3. socketpair 子端 (named fd 传入, fd 号经 entryParams 第三段传)
//    → 读线程: recv {magic(4B),len(4B),utf8} → 发 XIM COMMIT (Task 4)
// 4. X 事件循环: XPending 处理 SelectionRequest/ClientMessage (Task 3 状态机)
```

主进程侧 `xim_bridge.c`：`xim_bridge_channel_init()` 创建 socketpair、
子端经 NCP options 的 named fd 机制随 spawn 下发（锚定 xwayland spawn 的
fd 传递 API 面）；`xim_bridge_send_text()` 加锁写主端（`{0x57485349, len, bytes}`）。

- [ ] **Step 2: spawn 接线（display_compositor.cpp）**

`TrySpawnXimServer()`：`g_xwayland_ready_seen` 后触发一次；`OH_Ability_StartNativeChildProcess("libxim_server_child.so:Main", args, options, &pid)`；entryParams = `"<files>/xim_server_stderr.log|<xdgDir>|<fd号>"`。启动日志 `xim NCP spawned pid=`。

- [ ] **Step 3: 构建部署 + 判据**

```bash
set -o pipefail
rm -f entry/build/default/intermediates/libs/default/arm64-v8a/libentry.so
make NATIVE_ARCH=arm64-v8a hap 2>&1 | tail -2
bash scripts/package.sh deploy 192.168.0.206:33363 2>&1 | tail -1
hdc -t 192.168.0.206:33363 shell "aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.displayRoute x11"
sleep 30
hdc -t 192.168.0.206:33363 shell "hilog -x 2>/dev/null | grep -E 'xim NCP|xim-bridge' | tail -4"
```

Expected: `xim NCP spawned pid=` + 子进程日志（`xim-server: X connected` + `registered XIM_SERVERS`）。零回退门 3/3 PASS。

- [ ] **Step 4: Commit**

```bash
git add entry/src/main/cpp/display/ncp/xim_server_child.cpp entry/src/main/cpp/display/xim_bridge.c entry/src/main/cpp/display/xim_bridge.h entry/src/main/cpp/display/display_compositor.cpp entry/src/main/cpp/CMakeLists.txt entry/src/main/cpp/wine/wine_env.cpp
git commit -m "feat(display): XIM server NCP 子进程骨架 + socketpair 通道 + XMODIFIERS 下发 (XIM spec Task 2 NCP 版)"
```

---

### Task 3（落点子进程）: XIM 协议握手（CONNECT → OPEN → OPEN_REPLY → IC）

**Files:**
- Modify: `entry/src/main/cpp/display/ncp/xim_server_child.cpp`（ClientMessage 状态机；协议内容与原 Task 3 相同，落点从主进程 xim_bridge.c 挪到子进程）

**Interfaces:**
- Consumes: Task 2 的注册面（libX11 拿到 selection owner 后向 server 窗口发 `_XIM_PROTOCOL` ClientMessage）。
- Produces: `struct xim_client` 表（每 wine 进程一项：xconnect 窗、byte order、locale、IC 列表；≤8 项覆盖 explorer+应用并发）。

- [ ] **Step 1: 协议常量与状态机（真代码，同原 Task 3 全部内容）**

```c
#define XIM_CONNECT 1
#define XIM_CONNECT_REPLY 2
#define XIM_OPEN 30
#define XIM_OPEN_REPLY 31
#define XIM_CLOSE 32
#define XIM_CREATE_IC 20
#define XIM_COMMIT 69  /* minor = XIM_LOOKUPCHARS */
```

分派：CONNECT（byte-order 'l'/'B' + 版本 → 回 CONNECT_REPLY）→ OPEN
（locale "zh_CN"/"C" 都收 → OPEN_REPLY 带**最小 IM 属性表**：separator
`;\0` + input-styles 三组 `[(PreeditNone|StatusNone), (PreeditPosition|
StatusArea), (PreeditNothing|StatusNothing)]`，锚定 wine xim.c `xim_create`
的 style 过滤面实读）→ CREATE_IC（发 IC id）。包格式：ClientMessage 32B
内联小包 + `_XIM_PROTOCOL` property 超长包；组包/拆包 helper `xim_pkt_put/
get`（XIM wire 大端）。

- [ ] **Step 2: 构建部署 + 逐跳判据**

```bash
# +xim 跑批 (同原 Task 2 Step 2 命令), 收 wine stderr:
grep -E "trace:xim" /home/rianm/.claude/jobs/5a90fd84/tmp/wsx.log | tail -10
```

Expected: wine 侧 XOpenIM 成功路径 + 无 `Failed to open input method`；子进程日志 `xim-server: client connected locale=zh_CN`。停滞时按最后一跳打（桥日志规则）；协议存疑处一律以「wine 挂载成功」为验收。

- [ ] **Step 3: 零回退门（3/3 PASS）+ Commit**

```bash
git add entry/src/main/cpp/display/ncp/xim_server_child.cpp
git commit -m "feat(display): XIM 握手状态机 (NCP 子进程)——CONNECT/OPEN/IC 多 client (XIM spec Task 3)"
```

---

### Task 4: COMMIT + smoke ime 接入

**Files:**
- Modify: `entry/src/main/cpp/display/ncp/xim_server_child.cpp`（XIM_COMMIT 发送：读线程收到 socket 串 → 向焦点 IC client 发 COMMIT(XIM_LOOKUPCHARS, UTF-8)）
- Modify: `entry/src/main/cpp/display/xim_bridge.c`（`xim_bridge_send_text` 对外 API 定型）
- Modify: `entry/src/main/cpp/display/display_input.c` + `.h`（队列 `is_text` 分支 + `wl_ohos_input_post_text`，drain 调 send_text）
- Modify: `entry/src/main/cpp/display_route/display_route_napi.cpp`（`injectDisplayRouteText` NAPI）
- Modify: `entry/src/main/cpp/types/display_route_napi/Index.d.ts`（声明）
- Modify: `entry/src/main/ets/smoke/SmokeRunner.ets`（ime 动作 x11 分支 → `injectDisplayRouteText`）

**Interfaces:**
- Produces: `int xim_bridge_send_text(const char *utf8)`（Task 2 定型）——drain 调用点唯一变化。
- Produces: `void wl_ohos_input_post_text(const char *utf8)`——任意线程安全入口。

- [ ] **Step 1: 子进程 COMMIT + 主进程队列接线**（结构同原 Task 4：IC 焦点兜底 = `XGetInputFocus` 所属 client 的最近 IC；drain `is_text` → `xim_bridge_send_text` → free；NAPI/d.ts/SmokeRunner 三处与已撤实验同型）

- [ ] **Step 2: 判据——keyboard 转绿**

```bash
set -o pipefail
rm -f entry/build/default/intermediates/libs/default/arm64-v8a/libentry.so
make NATIVE_ARCH=arm64-v8a hap 2>&1 | tail -2
bash scripts/package.sh deploy 192.168.0.206:33363 2>&1 | tail -1
python3 automation/smoke.py run --job smoke/jobs/displayroute-win32-interactive.json \
  --display-route x11 --tests input-keyboard-x64,input-keyboard-x86 2>&1 | tail -4
```

Expected: 2/2 PASS，`chars` metric = `0041,0061,0031,4E2D`。乱码时按 Review Focus 3（编码协商）核对。

- [ ] **Step 3: 零回退门（3/3 PASS）**

- [ ] **Step 4: Commit**

```bash
git add entry/src/main/cpp/display/ncp/xim_server_child.cpp entry/src/main/cpp/display/xim_bridge.c entry/src/main/cpp/display/display_input.c entry/src/main/cpp/display/display_input.h entry/src/main/cpp/display_route/display_route_napi.cpp entry/src/main/cpp/types/display_route_napi/Index.d.ts entry/src/main/ets/smoke/SmokeRunner.ets
git commit -m "feat(display)+feat(smoke): XIM COMMIT 通道 (NCP) + ime 动作 x11 接入 (XIM spec Task 4)"
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
