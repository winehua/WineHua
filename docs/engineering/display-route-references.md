# 显示路线重构参考文档索引

> 适用场景：显示路线重构（spec: [../superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md](../superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md)）实现期间查资料时，先看这里，别满网找。
> 最后核实：2026-09-27
> 约定：`.temp/` 在 .gitignore 中（本地参考件，不入库）；本文件是唯一索引，新环境按 §3 的命令重新拉取。

## 1. 本地源码树（.temp/，含 in-tree 文档）

| 树 | 版本 | in-tree 文档/示例 | 开发中参考什么 |
|---|---|---|---|
| `wlroots/` | 0.21.0-dev (f141edc) | `docs/architecture.md`、`docs/env_vars.md`、`examples/simple.c`（最小合成器示例；tinywl 已移除）、`protocol/*.xml` | 后端/渲染器/buffer/seat API 的**头文件即权威**；M0 出图件照 simple.c 起步 |
| `gamescope/subprojects/wlroots/` | 0.20.2 上游 tag（我们钉版候选） | 同上结构 | 钉版基线；与 0.21 头文件 diff 即版本差异面 |
| `gamescope/` | Valve 最新 | `src/docs/`（含 Steam Deck Display Pipeline.png、Xserver-spec.xml） | M1 XWM/呈现接线、M2 present 管线；steamcompmgr.cpp = 无桌面策略合成管理先例 |
| `xserver/` | 主线 26.1.99.1 | `doc/`（Xserver-spec.xml 等）、`hw/xwayland/man/Xwayland.man`（命令行/env 权威） | M0 `-shm`/`-xkbdir` 参数、 glamor 开关；`xwayland-shm.c`/`xkb/ddxLoad.c` 已在 spec 引用 |
| `weston/` | 15.0.91 | — | 备选对照（已降级），仅 wlroots 裁剪失败时翻 |
| `wayland-protocols/` | fd.o 主线 | `stable/xdg-shell/xdg-shell.xml`、`unstable/pointer-constraints/`、`staging/`（cursor-shape、tearing、color-management…） | **协议 XML 即规范本体**：XWM↔合成器交互（xdg-shell）、输入捕获（M1）、IME（M2 text-input-v3）按此读 |
| `mutter/`、`winlator/`、`crossover/`、`proton/` | — | — | 背景参考：移动沙箱/兼容层先例。注：mutter 树的 text-input 是 Wayland 客户端侧实现，**无 Xwayland IME 桥**（2026-09-27 实查）——R-IME 补丁源需另行定位 |

## 2. 规范文档（.temp/docs/specs/）

| 文件 | 内容 | 何时读 |
|---|---|---|
| `ICCCM.html`（x.org X11R7.6 官方） | X11 客户端↔WM 约定：WM_TRANSIENT_FOR、WM_NORMAL_HINTS、selection、focus | M1/M2 窗口语义与模态落地时逐条对 |
| `EWMH.html`（freedesktop spec, latest） | 现代扩展：_NET_WM_STATE_MODAL、虚拟桌面、激活请求 | 同上；winex11 的行为判据 |

在线原版：ICCCM <https://www.x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html>；EWMH <https://specifications.freedesktop.org/wm-spec/latest/>

## 3. 新环境拉取命令

```bash
cd .temp
export https_proxy=http://192.168.1.2:7897   # 按当前代理调整
git clone --depth 1 https://gitlab.freedesktop.org/wlroots/wlroots.git
git clone --depth 1 https://gitlab.freedesktop.org/wayland/wayland-protocols.git
git clone --depth 1 https://gitlab.freedesktop.org/xorg/xserver.git xserver
git clone --depth 1 https://github.com/ValveSoftware/gamescope.git
git clone --depth 1 https://gitlab.freedesktop.org/wayland/weston.git
curl -sL -o docs/specs/ICCCM.html "https://www.x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html"
curl -sL -o docs/specs/EWMH.html  "https://specifications.freedesktop.org/wm-spec/latest/"
# gamescope 的钉版 wlroots: cd gamescope && git submodule update --init subprojects/wlroots
```

## 4. 在线权威文档（无法本地化的部分）

| 资源 | URL | 用途 |
|---|---|---|
| The Wayland Book | <https://wayland-book.com> | Wayland 核心概念/协议机制入门与查阅 |
| Wayland API doxygen | <https://wayland.freedesktop.org/docs/html/> | libwayland-server/client C API |
| 协议在线渲染（可读版） | <https://wayland.app> | 按 XML 生成的人类可读协议文档，含历史版本 |
| wlroots wiki | <https://gitlab.freedesktop.org/wlroots/wlroots/-/wikis/home> | 构建/迁移说明 |
| Xlib 手册（Nye / MIT） | <https://www.x.org/releases/current/doc/> | winex11 桥接时的 Xlib 细节 |

## 5. 使用纪律

- 协议行为争议：以 `wayland-protocols/` 的 **XML 注释原文**为判据（wayland.app 只是渲染）。
- 版本差异（0.20.2 vs 0.21）：以头文件 diff 为准，不凭记忆。
- 本文只维护索引；具体结论写进 spec 或代码注释，不在这里复制（同一判据只留一处）。
