# 多窗口模式 subsurface 按类分流 — 判据设计

日期：2026-09-10
状态：**实施中**——C++ 侧已完成并入库（6e28f28 按类分流 + 6736ebd 启发式退役 +
1d1005c ARGB 精确判透明），ArkTS 侧经审计无需改动（内嵌类不再产生 popup 事件，
`PopupWindowManager` 遍历自然空转），待设备验证。
适用范围：PC 融合模式 + Pad 多窗口模式（共用 `DisplayPolicy` 与 `PopupManager`）

## 0. 目的与不变量

把多窗口模式下"内嵌客户区"的 subsurface 合入主窗口帧（`WindowFrameComposer`），
"越界浮层"（菜单/下拉/子窗口）保留 popup 伪 toplevel 子窗口承载。**不变量**：

- **I1 输入不回归**：进入主窗口合成的 surface 必须是输入穿透的（客户区本来
  input region 为空，输入落在父窗口）——合成位置不改变输入命中结果。
- **I2 像素不裁剪**：可能越出窗口边界的 surface 一律不进主窗口合成
  （合入路径按窗口帧边界裁剪，越界部分会丢）。宁可多一个 popup，不可裁像素。
- **I3 判据无启发式**：类别判定只能使用协议级可观测量，禁止"尺寸相等""全屏
  父窗口"这类经验补丁（现有 `popup_manager.cpp:82-92` 的全屏补丁将随之消亡）。
- **I4 ZC 不退化**：zero-copy 层（GL 画面）无论如何承载，其直通路径与
  overlay 绘制语义保持不变。

## 1. 判据（协议级，commit 时可读）

### 客户区内嵌（→ 主窗口合成）：`inputRegionEmpty && vpSrcW <= 0`

| 观测点 | 客户区（winewayland client surface） | 依据 |
|---|---|---|
| `inputRegionEmpty` | 创建时设**空输入区**，声明"指针交给父窗口" | `thirdparty/wine/dlls/winewayland.drv/wayland_surface.c:1184-1192` |
| `vpSrcW <= 0` | **从不设 viewport source**（只设 destination=客户区尺寸） | 同文件 `:672-675`、`opengl_readback.c:502` |

### 浮层（→ 保留 popup 子窗口）：其余（`vpSrcW > 0` 或输入区非空）

菜单/未受管子窗口（WS_POPUP/WS_CHILD）走 `wayland_surface` + SUBSURFACE 角色
（`window.c:196-258, 451-505`），**每次 attach 必设 viewport source**
（`wayland_surface.c:488-490`，2 的幂对齐 padding 的真实尺寸），input region
正常（`window.c:226-229` 仅在 `WS_EX_TRANSPARENT|WS_EX_LAYERED` 时为空）。

### 已知例外的显式规则（不用启发式掩盖）

`WS_EX_TRANSPARENT|WS_EX_LAYERED` 子窗口同样 input region 为空但不是客户区
（罕见，非菜单）。**规则**：这类 surface 因 inputRegionEmpty 会被判"内嵌"，
但输入侧本就穿透（空输入区语义），像素侧若越界将由窗口帧裁剪——与现状
"系统子窗口不接收输入"一致，接受此边界并以日志可观测（见 §4）。

## 2. 数据流改动（每处一个命名决策点，不散落布尔）

```
UpdateSubsurfaceOnCommit(sd, ...)          // wl_core.cpp
  └─ DisplayPolicy::SubsurfaceRoute(...)   // display_policy.h 新命名查询
       ├─ Desktop（原 SubsurfaceAsLayer）  → UpdateSubsurfaceLayerOnCommit（不变）
       ├─ InlineClient（新）               → UpdateInlineSubsurfaceOnCommit（新）
       └─ Popup（原 else 分支）            → popupMgr_.UpdatePopupOnCommit（不变）
```

- `DisplayPolicy` 增加 `SubsurfaceRoute(hasInputRegionEmpty, hasViewportSource)`
  与类枚举 `SubsurfaceRouteKind { DesktopLayer, InlineClient, Popup }`，
  与既有五个命名查询同构（模式位不出现在调用点）。
- **Inline 路径**：帧数据进 `WindowFrameComposer` 已有的层列表机制
  （`desktop_compositor.cpp:302-383` 已为"窗口内层化"预留，含 ZC 层置顶），
  合成基底 = 窗口 SHM 帧，1:1 blit（`BlitWindowSubsurface`，无需缩放——
  与父帧同尺度，scale=1 语义见 `wl_core.cpp:352-353`）。
- 与 desktop 路径的差异仅在"坐标参照"：desktop 用虚拟屏幕坐标；窗口内用
  父窗口内容原点相对坐标（复用 `ComputePopupOffset` 语义）。
- **ZC 归属（实施时以日志确认后固化）**：`zc_bridge.cpp:177+` 的 PC 分支当前
  以 `rendererToplevelId == parentToplevel` 作为 ZC overlay 的承载条件，而现状
  popup renderer 的 id 是 popupId——需验证现状下 ZC 画面实际由哪个 renderer
  输出，再把承载点与 Inline/Popup 判定对齐，并补齐对应 renderer 的
  `GetLayerInfo` 几何（窗口内 = `ComputePopupOffset` 坐标）。

## 3. 保留 popup 的路径（浮层）不变

菜单类保持现状（伪 toplevel + `popup_show/move/resize/hide` + 独立子窗口），
但**全屏补丁退役**：`popup_manager.cpp:82-92` 的"父全屏 + off=(0,0) + 尺寸
相等"判定所识别的对象正是 InlineClient（客户区），改判据后该场景由 Inline
路径直接覆盖，补丁删除（同时删除其注释中记录的 war3 特例参数解耦——
Inline 的帧与窗口同尺度，不需要"窗口上报尺寸与内容尺寸解耦"）。

## 4. 可观测性（每类一条日志，字段足以人工复核类别）

- Inline 首帧：`[MW-SUBSURF] inline client surface key=... parent=#N size=WxH off=(x,y)`
- Popup 首帧：沿用 `[MW-POPUP] show ...`（现有字段已含 off/disp/win/buffer）
- 分流决策（debug）：`[MW-SUBSURF] route=inline|popup|layer key=... emptyInput=%d vpSrc=%d`

## 5. 实施顺序（对应任务 #148/#149/#150）

1. C++：`DisplayPolicy::SubsurfaceRoute` + `UpdateInlineSubsurfaceOnCommit`
   + 窗口内层列表接入（含 ZC 归属核对）；**PC 融合先行**（其
   `WindowFrameComposer` 已在跑，风险最低）。
2. 灰度：`winehua.fusionHost` 之外新增/复用调试键，可强制
   `route=popup` 回退（热切换不做，重启生效，与现有调试键同语义）。
3. 设备验证：E2/E3（PC 先）→ Pad → 回归矩阵（GL/ZC、菜单、拖动/缩放/全屏、
   多窗口 z 序、desktop 回归）。
4. ArkTS 收口：内嵌类不再建 popup 窗口（`PopupWindowManager` 只收浮层
   事件）；白屏修复的 `raiseWindowGroup` 对内嵌类自然失效（无 popup 可提升，
   主窗单窗口无 z 序问题）。
