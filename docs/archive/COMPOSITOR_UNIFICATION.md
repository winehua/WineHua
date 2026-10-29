# 合成管线统一抽象方案（Compositor Layer Unification）

> 状态：已完成（2026-09-22 归档）。文档写的是"全部未 push"，实际上四个阶段已全部合入 master，且它引用的 feature/compositor-layers 分支已删除。本文仅作历史记录。

> 状态: 阶段 1-4 已实施（feature/compositor-layers @76a2cd4/@d5deed7/@6df338a/@c2bd0ee），全部未 push
> 日期: 2026-08-01（更新: 2026-08-02）
> 范围: desktop（Pad）与 PC（模拟器）两种模式
> 关联: `ARCHITECTURE_OVERVIEW.md`、`OPENGL_VIRGL_DESIGN.md`、`CROSS_FORK_CONTRACTS.md`

## 1. 背景与问题

desktop 模式一帧画面由 **5 个互不相关的来源** 拼出（CPU 合成 3 个 + GPU 层 2 个），
每个来源有独立的几何、可见性、更新信号与状态机；层序由 **3 条独立规则** 叠加
决定（z-order 顺序、全屏独占跳过、遮挡重绘）。单实例场景（1 个 GL 窗口 + 普通
窗口）下 3 条规则恰好兼容；**多 GL 实例 / 多来源同时活跃时规则互相打架**，
已实证：2 个 GL 程序实例同时渲染时，被 zero-copy attach 的那个窗口的 GL 画面
无条件浮在最上面（详见 §4.1）。

PC 模式是另一套结构（每窗口一个渲染器 + 系统合成器管窗口间层序），同样存在
"窗口内多来源（CPU 帧 + ZC overlay）"与"N 份渲染状态实例化"的问题。

**结论**：脆弱性不是单点 bug，而是"内容来源多、层序规则散、状态无统一载体"的
结构性问题。需要一个统一抽象把这些收敛到单一数据模型上。

## 2. 现状盘点

### 2.1 一帧画面的来源（desktop 模式）

CPU 合成（`compositor/desktop_compositor.cpp` `TakeToplevelFrame`，315-684 行）合成出
"桌面帧"纹理，再上 GPU 层绘制：

| # | 来源 | 代码位置 | 内容 | 更新信号 |
|---|------|---------|------|---------|
| 1 | root 自身 SHM 像素 | 472 行 `out = rst->pixels` | 桌面壁纸/explorer 桌面，基底 | root dirty + `desktopRootFrameSerial_` |
| 2 | 子 toplevel SHM 像素 | 480-573 行 | 普通窗口，按 `toplevelZOrder_` 从低到高 blit | 每窗口 `dirty` |
| 3 | subsurface 层 | 576-654 行 | 菜单/popup/**GL readback 内容**，最后画、置顶 | `subsurfaceLayers_` 的 commit 更新 |

GPU 层（`egl_renderer.cpp` `RenderLoop`，572-965 行）在桌面帧之上再画：

| # | 来源 | 代码位置 | 内容 | 更新信号 |
|---|------|---------|------|---------|
| 4 | ZC GL overlay | 865-899 行 | 被 attach 的 GL 窗口 GPU 帧（`OH_NativeImage`） | `zeroCopyFrameAvailable_` + timestamp |
| 5 | 遮挡重绘 | 909-948 行 | 把 z-order 高于 ZC 层的窗口/菜单区域用桌面帧贴回 | 随帧实时算 `GetZeroCopyOccluders` |

### 2.2 一帧画面的来源（PC 模式）

- **每 toplevel 一个 XComponent + 一个 EglRenderer**（`plugin_manager.cpp:23`
  `CreateRenderer(toplevelId, surfaceId)`），N 个窗口 = N 个渲染循环、N 份 ZC
  状态机、N 个 VSync 线程。
- 窗口内：`TakeToplevelFrame(id)` 非 root 分支（`desktop_compositor.cpp:676-683`）
  只返回该窗口自己的 `st->pixels`；ZC overlay 以窗口局部坐标画在窗口内
  （`desktop_compositor.cpp:211-215` `rendererToplevelId == parentToplevel` 限定）。
- 窗口间层序由 HarmonyOS 系统合成器保证，不受我们控制；raise/移动经 ArkTS
  窗口 API 同步（MW-MOVE / RaiseToplevel 链路）。
- **窗口内层序未定义完整**：CPU 帧（含 subsurface）→ ZC overlay 最后画，
  同一窗口内 GL 内容画在菜单/弹层之上（当前依赖"GL 窗口菜单走 GDI 主 surface"
  才没爆）。

### 2.3 状态散落点（问题的根）

| # | 概念 | 散落位置 | 后果 |
|---|------|---------|------|
| S1 | 层序规则 | ① `toplevelZOrder_`（合成 480 行 / 输入 167 行）② 全屏独占（合成 487-491 行 `isZcGame` / 输入 45-127 行 fs-pick 分支）③ 遮挡重绘（`egl_renderer.cpp:909-948`） | 三规则各管一段，无单一权威；多实例时互相矛盾 |
| S2 | 可见性判定 | `IsToplevelVisibleLocked` 在合成（480、578 行）、输入（58、147、169 行）、遮挡计算（290、301 行）各调用一次 | 调用点分散，改一处漏一处 |
| S3 | 全屏目标选取 | 渲染侧 `desktop_compositor.cpp:394-398` 与输入侧 `input_resolver.cpp:56-60` **两处实现**，靠注释"与渲染侧同规则"维持 | 改一侧忘另一侧即输入/渲染错位 |
| S4 | 坐标变换 | CPU 合成直接像素；全屏 `FitMap` 变换（`geometry.h`）；ZC overlay letterbox 映射（`egl_renderer.cpp:888-891`）；遮挡重绘 UV 换算（934-938 行） | 四套变换必须同步（历史上已有 1px 偏差） |
| S5 | ZC 状态 | ① `zeroCopySurfaceKeys_`（compositor，合成跳过）② ready marker 文件（`graphics_broker.cpp:629`，guest 选路）③ `zeroCopyAttachedSurfaces_`（broker，host 认领） | fallback/恢复时三处状态互相追赶，任一滞后即黑屏/双层 |
| S6 | 更新信号 | CPU 通道（dirty + frame serial + composition signature）与 ZC 通道（frame available + timestamp）独立，`haveFrame` = 两者并（`egl_renderer.cpp:740`） | 双通道对齐依赖时序；错位一帧即闪屏/残留 |
| S7 | ZC attach 选择 | `TryAttachZeroCopySurface`（`egl_renderer.cpp:126-281`）轮询 broker 选**一个** unattached surface，单值 `zeroCopySurfaceKey_` | 多 GL 实例时只有一个是"一等公民"，其余走 readback |

## 3. 问题分类

| 类别 | 问题 | 严重度 | 实证 |
|------|------|--------|------|
| 结构 | 层序三规则叠加，多实例互相打架 | 高 | 双 GL 实例全屏时一个始终浮最上面 |
| 结构 | 遮挡重绘是"事后补救"，假设"遮挡源像素必在桌面帧里"；ZC 互挡时假设破裂 | 高 | 同上 |
| 结构 | 输入/渲染层序各自实现（S3、S2） | 中 | 2026-07 实测 notepad 被连带标全屏抢输入 |
| 时序 | ZC 三状态同步追赶（S5）、双通道对齐（S6） | 中 | fallback 8 次失败转 CPU 的竞态 |
| 实例化 | PC 每窗口一份渲染状态（S7 × N） | 中 | N 窗口 = N 状态机，语义重复 |

## 4. 统一抽象设计：CompositorLayer

### 4.1 核心思想

**所有内容来源都是一种 Layer；层序、可见性、几何、更新信号全部收敛到 Layer
上；合成与输入遍历同一个按 zIndex 排序的 Layer 列表。**

```cpp
struct CompositorLayer {
    enum class Type { Root, Toplevel, Subsurface, ZeroCopy };
    Type type;
    uint32_t toplevelId;       // 归属窗口 (Root 为 0)
    uint64_t surfaceKey;       // wl_surface 关联 (Subsurface/ZeroCopy)

    // -- 内容 (二选一) --
    std::vector<uint8_t> pixels;    // CPU 内容 (Root/Toplevel/Subsurface)
    GPUContentRef gpu;              // ZC: OH_NativeImage + 采样变换

    // -- 几何 (桌面坐标, 统一) --
    int x = 0, y = 0, w = 0, h = 0;
    bool fullscreen = false;        // 全屏提升的输入

    // -- 层序: 唯一权威 --
    size_t zIndex = 0;              // 单调分配; 全屏提升 = 重排 zIndex

    // -- 状态 --
    bool visible = false;
    uint64_t updateSerial = 0;      // 每次内容/几何变化 +1
    bool zcActive = false;          // ZC 层是否走 GPU 内容 (false=fallback 到 pixels)
};
```

### 4.2 层序规则收敛为一条

- **zIndex 分配**：root=0 < toplevel（按 `toplevelZOrder_` 派生，尾部=最顶）
  < 该窗口的 subsurface/ZC 层（挂在父窗口层内，subsurface 协议顺序）。
- **全屏提升**：fsWin（fsPriority 最大者）的 zIndex 临时提到最顶（任务栏
  pin 豁免保留）。全屏独占从"合成跳过 + overlay 不重绘两条特判"变成
  **一次 zIndex 重排**。
- **ZC 层 = 普通 Layer**：按 zIndex 参与排序，上层 Layer 自然盖住它 →
  **遮挡重绘机制整体删除**（不再是"补救"，是"自然覆盖"）。

### 4.3 合成统一

`TakeToplevelFrame` 变为单循环：按 zIndex 升序遍历 Layer 列表，逐个
blit/绘制。三层合一（toplevel 循环 + subsurface 循环 + GPU overlay）不再
存在；"ZC 区域跳过 SHM 内容"保留为 **ZC Layer 的内容语义**（该层 GPU 内容
自绘，CPU 帧在它区域留空），而非散落的 skip 特判。

### 4.4 输入统一

`InputResolver` 遍历同一 Layer 列表做 hit-test（高 zIndex 往下）。全屏
fs-pick、subsurface 命中、toplevel 命中合并为一个循环；S3 的"两处实现"
消失（渲染与输入同源遍历）。

### 4.5 更新信号统一

每个 Layer 自带 `updateSerial`；`compositionSignature` 改为 Layer 列表
几何摘要；`haveFrame` = 任一 Layer 的 serial 变化。双通道概念消失。

## 5. 实施阶段（渐进式，不推倒重来）

### 阶段 1：层序单一数据源（行为等价重构）✅ 已实施

> 提交: `feature/compositor-layers` @76a2cd4（仅代码，未 push）

- 建立 Layer 容器（vector 按 zIndex 排序），`subsurfaceLayers_` +
  `zeroCopySurfaceKeys_` + toplevel 合成输入全部映射为 Layer。
- 合成循环重构为单循环；全屏独占/跳过逻辑**原样保留等价形式**（不动行为）。
- 输入侧 `FindInputTargetAt` 同样映射到 Layer 遍历（保留 fs-pick 提前命中
  作为性能优化，语义不变）。
- 验收：双平台回归，合成输出逐像素等价（可复用 `automation/validate_frame.py`）。

实施要点:
- Layer 构建不过滤不可见 toplevel（`visible` 仅作标记, 合成/输入循环按标记
  跳过）— 合成签名仍对不可见 toplevel 混入 (id,0), 与旧行为完全一致。
- 全屏独占（isZcGame 填黑 / SHM 黑边 / fs-pick）与遮挡相关判定原样保留,
  仅遍历源统一为 Layer 列表。

### 阶段 2：ZC 入层（修复双 GL 实例 bug）✅ 已实施

> 提交: `feature/compositor-layers` @d5deed7（仅代码，未 push）

- egl_renderer 绘制顺序改为：CPU 帧（ZC 区域留空）→ ZC Layer（按 zIndex
  位置画）→ zIndex 更高的 Layer 区域从 CPU 帧贴回（= 现有遮挡重绘，但语义
  变为"上层自然覆盖"）。
- **删除 `egl_renderer.cpp:909` 的 `!zeroCopyFullscreen_` 条件**：全屏 ZC
  也走"上层覆盖"，结果是无遮挡，行为不变。
- **删除 `desktop_compositor.cpp:487-491` 的 `isZcGame` 跳过**：其它窗口
  正常合成，被全屏 ZC 层盖住；fsWin 是 SHM 窗口时其 zIndex 提升自然盖住
  ZC 层（双 GL 实例 bug 在此步修复）。
- 保留"被完全覆盖的层跳过合成"作为性能优化（一般化 `isZcGame` 跳过，按
  遮挡关系裁剪，`7c04cfe` 已有先例）。
- 验收：双 GL 实例全屏/非全屏互叠场景 + 单实例回归 + 输入命中回归。

实施要点（超出文档的两个扩展, 均由实测驱动）:
- **subsurface zIndex 挂父 toplevel 层内**（文档 §4.2 设计）: 实测发现
  GL 画面（readback subsurface）永远置顶、无法被其它窗口遮挡 — 这是旧
  合成"subsurface 最后画"的固有行为, 阶段 1 为行为等价保留。阶段 2 按
  §4.2 落地: subsurface 紧跟父窗口 zIndex, z-order 更高的 toplevel 自然
  盖住; parent=root/不在 z-order 的层保持尾部置顶（任务栏等, 防沉底）。
- **GetZeroCopyOccluders 按新层序过滤**: 仅父窗口 z-order 不低于 ZC 窗口
  的 subsurface 贴回（同窗口菜单仍在 ZC 层之上), 与 Layer 层序一致。

已实机验证: 层级遮挡 ✓ 全屏 ✓ 菜单 ✓（Pad, 2026-08-02）

### 阶段 3：PC 窗口内收敛 ✅ 已实施

> 提交: `feature/compositor-layers` @d18a8c0（仅代码，未 push）

- PC 窗口内合成也用 Layer 列表（Root=窗口帧 + Subsurface + ZC Layer），
  窗口内层序统一；窗口间仍交系统合成器（不接管）。
- ZC fallback 状态机收敛到 `CompositorLayer::zcActive` 单一字段
  （`zeroCopySurfaceKeys_` / ready marker / attached 三处合并）。
- 验收：PC 双窗口 + 各窗口 GL 渲染回归。

实施要点（与文档的差异, 均为实测/工程约束所定）:
- **窗口内 subsurface 当前恒空**: PC 模式 subsurface 全部转 popup 伪
  toplevel（`UpdatePopupOnCommit`）, `BuildWindowLayerListLocked` 的
  Subsurface 循环是预留结构（zIndex 紧随 Root, 窗口局部坐标）;
  窗口内合成输出 = 窗口 SHM 帧, 行为不变。
- **zcActive = 状态消费单一字段**: 合成/输入/遮挡重绘只读
  `CompositorLayer::zcActive`（由 compositor `zeroCopySurfaceKeys_` 派生,
  该集合是唯一权威）。broker 的 attached 集合（IPC 簿记, detach 命令
  依据）与 ready marker 文件（guest 跨进程选路通道, `opengl_readback.c`
  读取）机制保留, 不参与合成判定。
- **发布点收敛**: 三处状态的更新收敛到 `PublishZeroCopyActive` /
  `UnpublishZeroCopyReady` / `ClearZeroCopyCompositorKey` 三个幂等方法
  （egl_renderer 私有）, 替换原 4 个分散调用点; Release 路径不再无条件
  清理未发布的状态。
- **fallback 两步时序保留**: 先撤 ready（guest 立即切 SHM）→ 等
  shmCommitSerial 越过基线（新 SHM 帧已到）→ 再撤 compositor key（恢复
  合成）。不可合并为单次调用 — 合并会在追赶窗口期合成到 ZC 前的旧 SHM
  帧（文档 §2.3 S5 的"追赶"是协议设计, 非缺陷）。

验收（2026-08-03 实机）: PC 双窗口 + 各窗口 GL 渲染 ✓（模拟器 x86_64，阶段 4 版 HAP）；Pad desktop 主路径 ✓。

### 阶段 4（可选）：全屏目标单一化 ✅ 已实施

> 提交: `feature/compositor-layers` @c2bd0ee（仅代码，未 push）

fs-pick（渲染/输入）收敛为 Layer 列表上的纯函数，删除 S3 的重复实现。

实施要点:
- `PickFullscreenLayerLocked(layers)`：DesktopCompositor 成员，遍历同一
  Layer 列表取可见全屏窗口中 fsPriority 最大者，返回 toplevelId（0=无）；
  渲染侧（TakeToplevelFrame）与输入侧（FindInputTargetAt）两个调用点，
  原两处 8 行循环删除。state 由调用方锁内按 id 查询（pick 时已确认非空）。
- 行为等价（纯重构）：选取规则未变，输入侧 fs-pick 诊断日志保留。

验收（2026-08-03 实机, 与阶段 3 一起补）: 全屏场景 + 双窗口全屏互叠 + 输入命中 ✓（Pad desktop 主路径）；PC 模式 ✓（模拟器）。

## 6. 收益

| 项 | 现状 | 统一后 |
|----|------|--------|
| 层序规则 | 3 条独立规则叠加 | 1 条（zIndex 排序） |
| 遮挡重绘 | 事后补救特判（909-948 行） | 删除（自然覆盖） |
| 全屏独占 | isZcGame 跳过 + overlay 不重绘 | 1 次 zIndex 提升 |
| 输入/渲染层序 | 两处实现（S3） | 同一 Layer 列表 |
| 新来源接入 | 改合成 + 改输入 + 改状态三处 | 加一种 Layer 类型 |
| 双 GL 实例 bug | 存在（§4.1） | 阶段 2 自动修复 |

## 7. 风险与对策

| 风险 | 对策 |
|------|------|
| 合成是核心热路径，回归面大 | 阶段 1 行为等价 + 像素级回归脚本；每阶段双平台验证（记忆: 后台构建用裸命令防吞退出码） |
| 全屏 ZC 时性能（跳过逻辑删除后要多合成被盖窗口） | 遮挡裁剪优化保留（阶段 2 明确写出） |
| 遮挡重绘贴回源仍依赖"CPU 帧含最终像素" | 语义修正后该假设成立（ZC 内容永不进 CPU 帧，贴回源正确）；文档固化 |
| PC N 渲染器实例化状态 | 阶段 3 收敛；不合并渲染线程（架构变动过大，收益低） |

## 8. 相关代码地图

| 文件 | 角色 |
|------|------|
| `entry/src/main/cpp/compositor/desktop_compositor.cpp` | 合成主循环（5 来源中的 3）、ZC 层管理、遮挡计算 |
| `entry/src/main/cpp/compositor/desktop_compositor.h` | `SubsurfaceLayer`、`ZeroCopyLayerInfo`、`ZeroCopyOccluderRect` 定义 |
| `entry/src/main/cpp/compositor/toplevel_manager.h` | `ToplevelState`（帧/几何/全屏状态）、`toplevelZOrder_`、fsPriority |
| `entry/src/main/cpp/compositor/input_resolver.cpp` | 输入命中（全屏分支 + subsurface + zOrder 三段） |
| `entry/src/main/cpp/egl_renderer.cpp` | 渲染循环、ZC overlay、遮挡重绘（来源 4、5） |
| `entry/src/main/cpp/graphics_broker.cpp` | ZC attach/query/ready marker（状态 S5） |
| `thirdparty/wine/dlls/winewayland.drv/opengl_readback.c` | guest 侧 ZC/SHM readback 选路（349 行） |
| `entry/src/main/cpp/plugin_manager.cpp` | PC 每窗口 renderer 实例化 |
| `entry/src/main/cpp/compositor/geometry.h` | FitMap 变换（S4） |
