# XIM server 设计：x11 路线 IME/Unicode 文本通道

> 状态：设计提案（待用户 review；批准后进实施计划）
> 日期：2026-10-07
> 决策依据：用户 2026-10-07 拍板立项（keyboard 两项转绿 + 虚拟桌面形态内真实 IME 打字桥）。
> 否证依据：[gap-analysis §5](2026-10-07-display-route-x11-three-mode-gap-analysis.md)——
> X 键盘链 Unicode 注入三层实测否证（xkbcommon/xkbcomp 双编译器分叉 + xkm 缓存按名短路
> + xkbcomp 1.4.7 无 Unicode keysym 写法）；XTEST 同理不成立。
> 记忆锚点：xkeymap-dual-compiler-split。

## 1. 目标

x11 路线上把 Unicode 文本串（IME commit 语义）送进 wine，覆盖两个消费方：

1. **smoke 注入面**：`ime` 动作（input-keyboard 用例 char-cjk 断言）。
2. **产品面**：OHOS 输入法框架 → text-input-v3 → wlroots → X 侧通道 →
   winex11，使虚拟桌面形态内真实 IME 打字可用（D13 缺口的治本）。

**冻结行为**：wayland 路线 text-input 路径不动；X 键盘链物理键路径不动。

## 2. 原理选型

**为什么是 XIM server**：winex11 有完整 XIM 客户端（xim.c，87 处引用）——
XIM 的 commit 串**不经 keymap**（XIM 协议直接传 compound text / UTF-8 串，
wine 侧 `XmbResetIC`/commit callback → Imm 流程 → WM_CHAR）。这是
fcitx5/ibus 在 Xwayland 下的同款通道，行业标准答案。选型对比：

| 通道 | 判定 |
|---|---|
| keymap 扩展（已试） | **否证**——见否证依据 |
| XTEST | 否证（非 X client + keymap 无 CJK 条目） |
| D19 剪贴板桥 | 已建，仅粘贴语义（自绘窗 Ctrl+V 不产生 WM_CHAR），覆盖不了断言面 |
| **XIM server** | ✅ commit 串直达，wine 客户端现成；量级数百行（对齐 fcitx 的 XIM 前端最小实现面） |

## 3. 架构

```
[产品面]  OHOS IME → ArkTS inputMethod 框架 → text-input-v3 (wlroots 内置)
          → wlr_text_input commit 事件 → [主进程注入队列]
          → socket 通道 → [XIM 桥 NCP 子进程] → XIM 协议 → winex11 xim.c
          → ImmProcessKey/ImmCommitString → WM_CHAR

[smoke面] injectDisplayRouteText(text)
          → display_input 队列 (is_text) → drain → socket 写入 → 同上
```

**XIM 桥落点（X1 实测修正，2026-10-07）**：**NCP 子进程**
（`display/ncp/xim_server_child.cpp`，对齐 xclient_child 模式：NCP spawn +
命名 fd + entryParams）。原设计（合成器进程直连）已被 X1 探针否证：
`XOpenDisplay(":0")` 在 app 主进程**挂起不返回**（X socket 对主进程 mount
namespace 不可达；xclient_child 连接成功是因为 NCP 子进程与 Xwayland 同
命名空间，不能外推）。子进程内实现最小 XIM server：

- XIM 协议走 XIMSERVER 属性（XIM_TRANSPORT）：root 的 `_XIM_SERVERS`
  property 注册 + selection `XIM_SERVERS` owner 应答 + `XIM_PROTOCOL`
  ClientMessage 握手/commit（发现→CONNECT→OPEN→IC→COMMIT 最小状态机）。
- 目标窗选择：子进程内 `XGetInputFocus` 所在 client 的 IC（wine 单窗应用
  单 IC 兜底）。
- 主进程 ↔ 子进程通道：socketpair（NCP 命名 fd 机制，xwayland_child 五连
  先例），协议极简 = `{magic, len, utf8 bytes}`；主进程侧 drain 线程写
  socket（write 原子性足够），子进程读线程发 XIM COMMIT。
- winex11 xim.c 侧预期行为不变：应用 SetFocus+CreateCaret 后 wine 尝试
  XOpenIM（`@im=winehua`）——server 在位时 IM 挂载成功，commit → WM_CHAR。

**既有资产对位**：
- NCP 子进程骨架：xclient_child（X 连接已证）+ xwayland_child（命名 fd 五连）。
- 合成器侧 text-input：wlroots 内置 `wlr_text_input_v3`，需在 display 栈
  enable + 监听 commit（当前未接，接入 ~50 行，X3）。
- smoke 注入入口：`injectDisplayRouteText`（NAPI + display_input 队列
  is_text 分支），drain 改写 socket 而非直调（承载变更后的唯一接线差异）。

## 4. 数据流（smoke 面最小闭环）

```
SmokeRunner ime 动作 → injectDisplayRouteText("中")
  → display_input 队列 (is_text) → drain → XIM 桥 commit
  → X ClientMessage (XIM_PROTOCOL) → wine xim.c → IMM
  → WM_CHAR(0x4E2D) → 测试窗 g_chars[3] → char-cjk PASS
```

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| XIM 协议细节多（transport 分代、复合文本编码） | 锚定 fcitx 前端 + libX11 ximcp 实测行为；先只支持 UTF8 commit；smoke 用例先行验证最小闭环 |
| wine 侧 XIM 挂载条件（locale/修饰符） | LANG=zh_CN.UTF-8 已在 env（entryParams 实证）；XMODIFIERS 需下发 `@im=winehua`（wine_env 汇集点加一项，随 __env 下发） |
| 两实例冲突（合会话内 wine 多进程各开 IM） | XIM server 单实例按 display 注册；多 wine 进程共享同一 IM 连接（XIM 协议原生多客户） |
| 产品面 text-input enable 影响 wayland 路线 | wlroots text_input_v3 global 只在 display 栈（x11 链）创建；wayland 路线合成器不动 |
| ~~app 主进程 X 连接沙箱权限~~ | **已实测否证（X1，2026-10-07）**：主进程 XOpenDisplay 挂起不返回 → 桥承载改 NCP 子进程（见 §3）；socketpair 通道为新增面，命名 fd 机制有 xwayland_child 先例 |

## 6. 里程碑

| 阶段 | 内容 | 通过判据 |
|---|---|---|
| X1 ✅ | 沙箱探针（**已完成 2026-10-07，否定性结论**）：主进程 XOpenDisplay 挂起 → 承载改 NCP 子进程；探针调用撤除（提交 1392d60） | ~~探针注册成功~~ → 按停止条件转向 NCP 承载 |
| X2 ✅ | XIM 桥最小 commit 通道（NCP 子进程版）+ smoke ime 接入（**已完成 2026-10-08，提交 a9c53a7**：OPEN_REPLY 段长 + XICATTR 全集声明等协议栈九处修复，chars=0041,0061,0031,4E2D） | input-keyboard-x64/x86 char-cjk 转 PASS ✅ |
| X3 | 产品面 IME 接入。**2026-10-08 平台现实修订**：`wlr_text_input_v3` 接线在本平台没有客户端——OHOS 系统 IME 是系统服务，不经 wayland 连到应用内合成器。软键盘场景由 D13 桥覆盖（insertText → ASCII 折 evdev / CJK 走 D19 剪贴板桥），硬件键盘组合输入同走 inputMethod 框架的 insertText 回调、同一座桥。text-input 接线仅在出现真实 wayland text-input 客户端时再评估 | 真机软键盘/实体键 IME 打字「中」进 notepad（手测；XMODIFIERS 下发已随 X2 落地） |

X2 通过前不开 X3。虚拟桌面零回退判据同 M4 §7。

## 7. 明确不做

- preedit/候选窗语义（产品面候选 UI 走 OHOS 输入法框架自带，X 侧只见 commit）——
  与 wayland 路线 text-input 现状对齐（合成器也只处理 commit）。
- XIM transport 的 XIM_EXT / 多屏 / locale 组合枚举——按 wine xim.c 实际
  请求面实现，不预做。
