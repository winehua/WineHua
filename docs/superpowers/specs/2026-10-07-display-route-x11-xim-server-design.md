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
          → wlr_text_input commit 事件 → [XIM 桥] → XIM 协议 → winex11 xim.c
          → ImmProcessKey/ImmCommitString → WM_CHAR

[smoke面] injectDisplayRouteText(text)（复用已撤实验的入口签名）
          → [XIM 桥] 直接走 commit 分支 → 同上
```

**XIM 桥落点**：新文件 `display/xim_bridge.c`，跑在合成器进程，作为 **X
client** 连本机 Xwayland（`XOpenDisplay(":0")`——合成器进程此前无 X 连接，
这是新增面；权限面 = Xwayland 本地连接，与 xclient_child 同款）。
实现最小 XIM server：

- XIM 协议走 XIMSERVER 属性（XIM_TRANSPORT）：XIM server 之间经
  `_XIM_SERVERS` root property 注册 + XIM 事件流（XIM_PROTOCOL via
  ClientMessage）。最小实现面：注册 → 接受 IM 开启 → RECEIVE seen →
  commit 串投递（`XIM_COMMIT`，XIM compound text / UTF-8 编码）。
- 目标窗选择：smoke 面 = 当前 seat 键盘焦点 xs 的 X window；产品面 =
  text-input 的 focused surface 对应 xs。
- winex11 xim.c 侧预期行为：应用 SetFocus+CreateCaret 后 wine 会尝试
  XOpenIM——server 在位时 IM 挂载成功，commit → WM_CHAR（用例断言面）。

**既有资产对位**：
- 合成器侧 text-input：wlroots 内置 `wlr_text_input_v3`（M0 spec 选型时已
  确认内置），需在 display 栈 enable + 监听 commit（当前未接，接入 ~50 行）。
- smoke 注入入口：恢复 `injectDisplayRouteText`（NAPI + display_input 队列
  is_text 分支），消费点改调 XIM 桥（实验代码结构可复用，机制换 XIM）。

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
| XIM 协议细节多（transport 分代、复合文本编码） | 锚定 fcitx 前端 + 上游 wine xim.c 实读；先只支持 UTF8 commit（XIM_EXT_MOVE 之类不碰）；smoke 用例先行验证最小闭环 |
| wine 侧 XIM 挂载条件（locale/修饰符） | LANG=zh_CN.UTF-8 已在 env（entryParams 实证）；XMODIFIERS 需下发 `@im=winehua`（wine_env 汇集点加一项，随 __env 下发） |
| 两实例冲突（合会话内 wine 多进程各开 IM） | XIM server 单实例按 display 注册；多 wine 进程共享同一 IM 连接（XIM 协议原生多客户） |
| 产品面 text-input enable 影响 wayland 路线 | wlroots text_input_v3 global 只在 display 栈（x11 链）创建；wayland 路线合成器不动 |
| 合成器进程起 X 连接的沙箱权限 | M0 式真机首验项：XOpenDisplay(":0") 在 app 进程的可行性探针（xclient_child 已证同进程族可行） |

## 6. 里程碑

| 阶段 | 内容 | 通过判据 |
|---|---|---|
| X1 | 沙箱探针：app 进程 XOpenDisplay + `_XIM_SERVERS` 注册可见性 | 探针程序在设备端输出注册成功 + xprop 可见 |
| X2 | XIM 桥最小 commit 通道 + smoke ime 接入 | input-keyboard-x64/x86 char-cjk 转 PASS（13 前缀 13/13） |
| X3 | 产品面：text-input-v3 接入 + XMODIFIERS 下发 | 真机软键盘/实体键 IME 打字「中」进 notepad（手测） |

X2 通过前不开 X3。虚拟桌面零回退判据同 M4 §7。

## 7. 明确不做

- preedit/候选窗语义（产品面候选 UI 走 OHOS 输入法框架自带，X 侧只见 commit）——
  与 wayland 路线 text-input 现状对齐（合成器也只处理 commit）。
- XIM transport 的 XIM_EXT / 多屏 / locale 组合枚举——按 wine xim.c 实际
  请求面实现，不预做。
