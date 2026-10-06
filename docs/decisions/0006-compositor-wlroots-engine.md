# 0006 — fusion 路线合成器引擎：wlroots 直接服务 winewayland.drv

- 状态：方向已定（用户 2026-10-06 拍板）；细则 spec 待出，出前有两个开放问题（§7）
- 决策日：2026-10-06
- 关联：取代 [0001-self-built-compositor.md](0001-self-built-compositor.md) 的「继续自研」结论在 fusion 路线上的余留效力；与 [2026-09-27 x11 spec](../superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md) 并行不悖（x11 路线继续作为独立线，本决策只处置 fusion 引擎）

## 1. 决策

自研 wayland compositor（`entry/src/main/cpp/compositor/` 约 1.1 万行协议与合成代码）**由 wlroots 直接替换**：wlroots 作为 wayland 服务端承接 winewayland.drv，ArkTS 窗口管理层保留。验收标准 = **wine 的 wayland 体验尽可能完整**（逐条对照 winewayland.drv 的协议 bind 名单 + 真实 wine 程序行为）。

**校准目标声明：本决策只对 wine 体验完整性负责，不以 x11 路线为校准目标。** x11 路线自身仍有未解问题（D23 guest GL 面挂接、§1.6 虚拟桌面不可用、§2.6 启动期同步基线异常等），它既不是本工作的参照系，也不是依赖项；winewayland.drv 是 wine 自己的现代原生驱动，把它服务正确本身就是终值，不需要借道 x11 的完成度。

mutter 候选否决，weston 维持既有降级（见 §4）。

## 2. 前提校准（为什么现在做这个决策）

- 「x11 收口 → M3 旧合成器退役」（x11 spec:315）只是**驱动 x11 路线搭建的目标设定，不是产品规划**。fusion/wayland 是长期产品线（平板虚拟桌面为 0004 钉死的默认形态；退役需三形态各有替代，单形态可用 ≠ 退役条件）。
- 因此自研 compositor 不能指望「整线退役」了结，必须真正修好。用户实测定性：它「看似实现完整，实际细节处理很有问题，没能完整支持 wine 用到的特性，有的实现根本是错的」。
- 机制根源：这 1.1 万行是对着单一客户端（wine）手搓的协议实现，没有第二个客户端帮它暴露 bug；同批协议在 wlroots 里被 sway/labwc/gamescope/cage 等数十个 compositor 消费了十年。

## 3. 协议覆盖证据（决策核心依据）

对照基准 = `thirdparty/wine/dlls/winewayland.drv/wayland.c:93-246` 的 bind 名单（必需 5 项硬检查 + 其余 presence-check 可选），wlroots 能力以 `thirdparty/wlroots/include/wlr/types/`（0.20.2，树内核实）为准：

| global | 自研 server 现状 | wlroots 0.20.2 |
|---|---|---|
| `wl_compositor` v4 / `wl_shm` / `wl_subcompositor` / `wp_viewporter` / `xdg_wm_base`（必需） | 手搓 | 原生 |
| `wl_seat` ≤8（pointer/keyboard/touch） | 手搓（set_cursor 空实现 `seat.cpp:25-28`） | 原生 |
| `wl_output` | 有 | 原生 |
| `zxdg_output_manager` ≤3 | **缺** | `wlr_xdg_output_v1` |
| `zwp_text_input_v3` | 手搓 277 行 | `wlr_text_input_v3` |
| `pointer_constraints` + `relative_pointer` | 手搓 468 行 | 原生两件 |
| `wl_data_device` v2 + `zwlr_data_control`（剪贴板） | **全缺**（D19 绕 X selection 的根因） | `wlr_data_device` + `wlr_data_control_v1` |
| `wp_cursor_shape` ≤2 | **缺** | `wlr_cursor_shape_v1` |
| `xdg_toplevel_icon` | **缺** | `wlr_xdg_toplevel_icon_v1` |
| `zwp_linux_dmabuf` | 缺（GPU 走 ZC 旁路） | `wlr_linux_dmabuf_v1` |
| `wp_pointer_warp` | 缺 | **0.20.2 无**（可选 bind，wine 缺省降级；钉 0.21+ 可跟进） |
| `winehua_toplevel` v1（模态属主，私有） | 有 | 需重挂（wlroots 支持自定义 global，小活） |

结论：**16/17 原生覆盖**；替换直接带来四个从未有过的特性类（wayland 剪贴板、真光标、任务栏图标、xdg-output），并把 text-input/constraints/relative-pointer 从手搓换久经检验。

## 4. 候选裁决

- **wlroots**：在位者而非候选。0.20.2 已静态链入 `libentry.so`（x11 路线在用），3 个 out-of-tree 补丁，OHOS 呈现链（OH_NativeBuffer allocator → headless output → wlr_scene → 零拷贝 GL present，OH_NativeVSync 111-114fps 实测）已建成验收。同版先例 gamescope（BSD-2，`gamescope/subprojects/wlroots` 即 0.20.2）。
- **mutter：否决**。`​.temp/mutter/COPYING` 实锤 GPL-2.0-or-later（闭源 HAP 内嵌许可证结构性冲突）；依赖 gtk4/libadwaita/libinput/libudev/libelogind/colord/gnome-desktop-4 等 OHOS host 全缺；`MetaBackendNative` 需 libinput+udev+logind、`Nested` 需父 compositor，两前提 OHOS 均不成立，须自写第三种 backend 并永续跟随 GNOME 半年节奏。
- **weston**：维持参考索引既有降级（「仅 wlroots 裁剪失败时翻」），未触发。

## 5. 目标架构

```
wine (winewayland.drv，基本不动；__OHOS__ 私有协议保留)
  → wayland socket
    → wlroots 0.20.2（xdg_shell 服务端 + wlr_scene 合成 + seat，原生）
      → 策略层（唯一自养，薄）：窗口↔output 映射、OHOS 窗生命周期
        → 复用 display/ 呈现链（ohos_output.c + display_input.c）
```

- 平板虚拟桌面 = 单 output 全合成：wlr_scene 原生 GPU 合成，替换 `frame_pipeline.cpp` 885 行 CPU blit——compositor.md 已知五问题（叠放隐患/全屏黑边/resize 不刷新/直传半透明丢失等）全是 CPU blit 路径的病，模型上消除。
- PC 多窗口 = 每 xdg_toplevel 一个 headless output ↔ 每窗画布（形态与 x11 阶段 C 相似，但按 wine 客户端语义独立设计，不继承 x11 的实现与问题清单）。
- ZC 游戏旁路（GraphicsBroker → NativeWindow）在 wayland 协议之外，不受影响。
- ArkTS 窗口管理层（22 种 toplevel 事件、WineWindowManager、任务栏）保留，事件源从自研 event bus 换成 wlr xdg_shell 事件转发。
- **代码级复用与校准分离**：`display/` 的 OHOS 胶水（output/present/seat 桥）是同进程同仓库的复用候选，但其服务对象从 xwm 换成 xdg 客户端，复用必须逐段重新校验；display/ 自身的未解问题（D23 挂接类）**不随迁**——凡进本路线的代码按 wine 体验标准重新验收。

**组件处置**：`compositor/` 9458 行 + `seat.cpp`/`input_manager.cpp`/`text_input.cpp`/`pointer_extras.cpp` 约 1800 行 → 退役；`egl_renderer` 每窗渲染 → ohos_output 呈现链顶替；ArkTS 层保留换事件源。

## 6. 风险

1. 工作量 M0+M1 同量级（数周）：`display/` 胶水可作复用起点（§5），但按「复用与校准分离」原则需逐段重验，xdg 事件的窗口策略层（z-order/模态/popup→OHOS 窗映射）是真活——成本估算不因复用而乐观。
2. configure/serial 生命周期与自研即时应答不同，wine 的响应模式需实测调校——「体验完整」的主战场。
3. 引擎替换**就地**进行（用户 2026-10-06 已裁决，见 §7.1）：同一 socket 路径，ArkTS 无感，不出现第三栈。
4. pointer-warp 缺失仅影响个别绝对 warp 场景（可选 bind）；钉 0.21+ 回头补。

## 7. 开放问题（spec 前需裁决）

1. ~~就地替换 vs 第三路线并存~~ **已裁决（用户 2026-10-06）：就地替换**——fusion 引擎换成 wlroots，`displayRoute` 仍两态（wayland/x11），不出现第三栈。
2. 平板虚拟桌面（B2）与 PC 多窗口（B3）的优先序——建议先平板（0004 默认形态，且 B0→B2 是同一条 output 路径的自然延伸），待用户确认。

## 8. 里程碑草案

- **B0**：wlroots 起 xdg 服务端，notepad 经 winewayland.drv 上屏（单 output + 现成呈现链）——协议正确性第一证。
- **B1**：输入全通（pointer/keyboard/text-input），拖拽/resize/双击（复用 D15 手势层，输出端换 wlroots seat 注入）。
- **B2**：平板虚拟桌面完整形态（scene 合成 + 任务栏事件）+ 剪贴板（data-control）+ 真光标（cursor-shape）。
- **B3**：PC 多窗口（每窗 output）+ winehua_toplevel 模态重挂 + 长尾样本验收。

> 后续 spec 落于 `docs/superpowers/specs/`，本文只承载方向裁决与证据；实现结论写进 spec 与代码注释，不在这里复制（同一判据只留一处）。
