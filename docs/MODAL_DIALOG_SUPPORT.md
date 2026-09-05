# Modal Dialog 支持（虚拟桌面 + PC 多窗口）设计文档

> 日期: 2026-09-04 · 状态: 已实现未提交 · 涉及: thirdparty/wine (winewayland.drv) + 主仓 (compositor/ArkTS)

## 问题

Wine 的模态对话框（MessageBox / DS_MODALFRAME 对话框）是普通 `xdg_toplevel`，
Wayland 无模态概念，导致：

- **虚拟桌面（Pad 桌面合成）**：点击主窗口 → ArkTS `raiseToplevel(owner)` 把主窗口
  抬到对话框上面 → 主窗口因 owner-disabled 不可点、对话框被遮挡且无法切到前台，
  死局。
- **PC 多窗口**：每个 Wine 窗口是独立 UIAbility 主窗口，OHOS 侧主窗口"以独立
  任务卡片显示"——模态对话框占一个任务栏图标，且窗口间无父子/模态语义。

## 方案

### 1. wine 侧：探测 + 私有协议上报（`__OHOS__` 隔离，最小侵入）

- 新增私有协议 `winehua-toplevel`（wine 侧 `dlls/winewayland.drv/winehua-toplevel.xml`，
  宿主编译时从同一 XML 生成 server 端）：
  - `set_modal(wl_surface surface, wl_surface owner_surface | NULL, uint32 modal)`
- 探测判据 = Win32 模态语义：窗口带 `WS_EX_DLGMODALFRAME` + owner 存在 + owner 带
  `WS_DISABLED`（模态创建时序：`dialog.c` 先 `EnableWindow(owner, FALSE)` 再显示
  对话框，MessageBox 同路径）。
- 上报点：
  - `WAYLAND_WindowPosChanged`（窗口显示/移动时探测，此时 owner 已被禁用）
  - `wayland_surface_destroy`（销毁前解除）
- 状态记忆（`surface->winehua_modal_owner`）→ 仅变化时发。
- 改动清单（全部 `#ifdef __OHOS__`）：`waylanddrv.h`（include + 2 字段 + 2 声明）、
  `wayland.c`（registry bind）、`window.c`（6 行调用）、`wayland_surface.c`（4 行调用）、
  `Makefile.in`（2 行）。新增文件：`modal.c`、`winehua-toplevel.xml`。

### 2. 宿主 compositor（虚拟桌面模式）

- `SurfaceData` 暂存 modal 关系（set_modal 早于 get_toplevel 到达时）；
  `ToplevelManager` 增加 `modalOf_`（owner → 列表）+ `ToplevelState::modalOwnerId_`
  组员化。
- **modal 不入 z-order 数组**：`BuildLayerListLocked` 在 owner lane 内展开
  （itemSeq 从 100000 起，不与 subsurface 子层冲突；递归支持嵌套对话框）→
  模态框恒在 owner 上方（Win32 owned 窗口语义）。
- `RaiseToplevel(modal)` 内部换成 raise 它的 owner（组提升）；commit 路径
  `EnsureInZOrder` 跳过组员。
- **输入拦截**（`InputResolver` + `input_manager`）：命中"被模态禁用的 owner" →
  `blockedModalId` 非 0 → 吞 PRESS + 键盘焦点切到模态框 + raise 组（Win32 首次
  点击只激活模态框的语义）。MOVE/RELEASE 透传（防相对模式基线失真/按钮状态卡死）。
  全屏窗口（游戏的系统弹窗）走同一命中循环，天然兼容。

### 3. PC 多窗口模式

- host 经既有事件通道发 `modal` 事件（json: modal/owner/tl/dx/dy/w/h，dx/dy 为
  modal 相对 owner 的桌面坐标差；事件先于首帧的 `created`）。
- ArkTS `ModalWindowManager`（新）：在 **owner 的 WindowStage** 内
  `createSubWindowWithOptions(..., {isModal: true})` + `loadContent('pages/WineWindow')`
  （XComponent/renderer/键盘/触控全复用）→ OHOS 模态子窗口语义：
  - 辅助窗口不进任务栏/任务卡片（主窗口才进）
  - 父级窗口不能响应用户操作，直到子窗口关闭（与 Win32 模态一致）
- `WineWindowManager`：modal 事件的窗口**不启动 WineWindowAbility**（否则又是
  独立主窗口）；resize/destroyed 事件转发 ModalWindowManager；owner 销毁级联
  销毁其模态子窗口。
- 位置：owner 屏幕 rect + (dx,dy)×scale；尺寸 wine 逻辑 × scale。

## 明确不做（已知限制）

- **MB_TASKMODAL**（无 owner 的线程级模态）：协议无"禁多个窗口"表达，使用量低。
- 非模态 owned 窗口没有强制置顶（只保证 modal；owned 窗口顺序维持现状）。
- APP_MODALITY 不用：只 WINDOW_MODALITY（禁父窗口），对齐 Win32。

## 补充修复（code review 后）

- 模态组随 owner 移动：`ApplyModalDeltaLocked`（move grab 拖动 / wine geo 同步
  两条路径）同步整条 modal 组，Win32 owned 窗口跟随 drag 语义。
- 拦截路径 raise 走 `WaylandServer::RaiseToplevel`（持锁 + 任务栏 PinToTop +
  桌面 dirty，与 ArkTS 用户 raise 同语义）。
- 另修复清单见 git blame：wine 侧锁死（get_nolock）、modal 层 fullscreen 位
  清零（全屏游戏确认框）、嵌套模态拦截早退、owner 先亡组清理、PC 模式
  owner 未就绪暂存/重发 reposition/幽灵 entry 守卫。

## 冒烟验证（真机）

- 虚拟桌面：notepad → 文件-退出 · 无保存 → 对话框出现；点击 note 主窗口任意处
  → 对话框浮起并获得焦点（hilog `MODAL-BLOCK` / `Input MODAL-BLOCK`）；点确定/
  取消恢复。
- PC 模式：同场景 → 无保存对话框不出现在任务栏，主窗口点不动，关对话框后恢复。
- 回归：普通窗口点击/移动、游戏全屏弹窗、explorer 右键菜单（TopAnchored 不受
  modal 展开影响）。
