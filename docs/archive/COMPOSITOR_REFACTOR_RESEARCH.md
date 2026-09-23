# 合成管线重构前调研报告

> 状态：调研结论已被采用（2026-09-22 归档）。文中的行号以 2026-08-01 调研当日的代码为准，与当前代码有偏移。

> 状态: 调研完成（2026-08-01）
> 目的: 为 `COMPOSITOR_UNIFICATION.md` 的 4 阶段重构提供行为级基线，保证行为等价重构的可验证性
> 范围: `desktop_compositor.cpp` / `egl_renderer.cpp` / `wl_core.cpp` / `input_resolver.cpp` /
>        `toplevel_manager.*` / `graphics_broker.cpp` / `plugin_manager.cpp` / guest `opengl_readback.c`
> 行号基准: 本报告内所有行号以调研当日代码为准（feature/render-element-completeness）

---

## 1. 帧管线全景

### 1.1 线程模型

| 线程 | 职责 | 节拍 |
|------|------|------|
| Wayland 线程 | `wl_event_loop_dispatch`（wl_core.cpp surface_commit 等协议请求）、`wl_display_flush_clients` | 50ms timeout（wayland_server.cpp:105） |
| 渲染线程 ×N | 每 renderer 一个 `EglRenderer::RenderLoop` | NativeVSync（60-120Hz），失败时 deadline fallback 16.6ms（egl_renderer.cpp:641-709） |

**desktop 模式只有 1 个 renderer**（root renderer，`PluginManager` 单例管理，root 重建时 `MoveRendererToToplevel` 迁移，plugin_manager.cpp:102-113）。PC 模式每 toplevel 一个（plugin_manager.cpp:23 `CreateRenderer`）。

**锁约定**：所有合成/层序状态（`subsurfaceLayers_`、`zeroCopySurfaceKeys_`、`toplevelZOrder_`、`ToplevelState`）统一由 `ToplevelManager::toplevelMutex_` 保护；`DesktopCompositor` 自身**不带锁**，靠"调用方已持有 tmgr mutex"约定（desktop_compositor.h:34 注释）。渲染线程每次 `TakeToplevelFrame` 都先 `tmgr_.Lock()`（desktop_compositor.cpp:355）。

> **对重构的约束 1**：Layer 容器必须沿用同一把锁，或在设计时显式论证独立锁与嵌套序（合成函数内部已持锁，销毁路径 wl_core.cpp:108-118 也在锁内调 `RemoveSubsurfaceLayer`——独立锁若加在这条链上会引入锁序问题）。

### 1.2 渲染循环每帧流程（egl_renderer.cpp:714-961）

```
TryAttachZeroCopySurface(useToplevel)   // 100ms 节流查询（169 行）
zeroCopyGeometryFrame = zeroCopyGeometryDirty_
UpdateZeroCopyFrame(w, h)               // 消费 GPU 帧
cpuFrame = TakeToplevelFrame(rootId, px, fw, fh)
haveFrame = cpuFrame || zeroCopyFrame || zeroCopyGeometryFrame   // 740 行
   ↓ 无新帧且 surface 尺寸未变 → 跳过 GPU 绘制（788-800，静态桌面省电）
letterbox = ComputeFitRect(surface, frame)   // 814 行
glClear → 画 CPU 帧纹理（856-863）→ 画 ZC overlay（865-899）→ 遮挡重绘（909-948）→ eglSwapBuffers
```

关键语义：
- `px`（CPU 帧 out buffer）由渲染线程持有，**跨帧复用**——`TakeToplevelFrame` 在 out 上做增量叠加（见 1.3）
- 桌面模式 `useToplevel = GetDesktopRootToplevelId()`（726 行），root 重建后自动跟随
- 无新帧但 surface 尺寸变化 → 强制重绘（re-letterbox，786-799 注释：`ForceToplevelRedraw` 的 dirty 与 buffer 异步切换存在竞争窗口，靠 surface 尺寸探测兜底）

### 1.3 dirty 与 compositionSignature 两层机制（desktop_compositor.cpp:430-478）

**`st->dirty`** = "有内容变化，需要重新合成"（commit/raise/全屏切换/mark root dirty 等置位，`TakeToplevelFrame` 消费后清除）。

**`compositionSignature`** = "基底是否可复用"：
- mix 内容：root id/尺寸 + 每个 zOrder 成员的 (id, 可见性, x/y/w/h, fullscreen) + 每个 subsurface 层的 (surface 指针, 是否 ZC, parentToplevel, 可见性, 几何, vpDst)（435-464）
- `rebuildBase` 触发条件（467-470）：未初始化 / out 尺寸不匹配 / **root commit serial 变化**（`IncrementDesktopRootFrameSerial`，root commit 时 +1，wl_core.cpp:583-585）/ 签名变化
- `rebuildBase=true` → 整帧重拷 root 像素（472 行，rootW×rootH×4 字节）；false → 在现有 out 上重新叠加全部可见 children + subsurfaces

> 推论：基底重建的语义 = "root 像素或层序几何变了"。注意**签名不含 subsurface 的像素内容/opaque/damage**——这些变化靠 dirty 信号（commit 置位），不靠签名。Layer 化后 `updateSerial` 必须同时覆盖这两类（几何进签名、内容进 dirty）或合并为单一信号并验证增量重建语义。

### 1.4 性能基线（desktop_compositor.cpp:315-352 已有测时统计）

`[GL-TAKE]` 每 120 样本输出 6 段 avg/max：lockWait / rootCopy / children / subsurfaces / output / total。

合成每帧拷贝量（估算）：
| 段 | 数据量 | 触发条件 |
|----|--------|---------|
| root 基底 | rootW×rootH×4 | 仅 rebuildBase 时 |
| children blit | Σ 可见窗口 w×h×4 | 每帧全量（无 damage 裁剪，480-573） |
| subsurface blit | Σ 层 (damage 裁剪后) | 每帧，damage 包围盒裁剪（616-628） |
| 全屏 ZC 跳过 | 0（省掉全部 children + subsurface） | `isZcGame`（487-491）+ 580 行跳过 |

> **对重构的约束 2**：阶段 2 删除 `isZcGame` 跳过后，"被盖层跳过合成"必须保留为遮挡裁剪优化（文档 §5 阶段 2 已写），否则全屏 ZC 场景每帧多出全部 children+subsurface 拷贝。实测成本由 `[GL-TAKE]` 日志对比。

---

## 2. 内容来源行为档案

### 来源 1：root SHM（桌面基底）

| 项 | 事实 |
|----|------|
| 数据 | `ToplevelState.pixels`（root toplevel），commit 时 `CopyShmContentTight`（wl_core.cpp:580） |
| 更新信号 | commit（serial+1，wl_core.cpp:584）+ `rst->dirty` |
| 合成规则 | 基底：rebuildBase 时整帧拷入（472）；无 children/subsurfaces 时直接透传（375-381） |
| 输入规则 | `IsToplevelVisibleLocked(rootId) = false` 恒不可见（toplevel_manager.cpp:16）；兜底命中返回 root surface（input_resolver.cpp:186-190） |
| 特殊 | root 重建时 `desktopRootToplevelId_` 复位再置位（wayland_server.cpp:223-227）；`GetZeroCopyLayerInfo` 要求 renderer 是 root（153 行） |

### 来源 2：toplevel SHM（普通窗口）

| 项 | 事实 |
|----|------|
| 数据 | `ToplevelState.pixels/w/h/x/y/wineX/wineY/shmFormat/dirty` |
| 更新信号 | commit（wl_core.cpp:572-704）；`hasPosition` 首帧置位（597-606） |
| 合成规则 | 按 `toplevelZOrder_` 从低到高 blit（480-573）；ARGB 逐像素 alpha 混合（546-572）；部分越界裁剪（537-545） |
| 输入规则 | zOrder 从高到低命中（167-184）；`inputRegionEmpty` 穿透（176） |
| 可见性 | `IsToplevelVisibleLocked`：非 root / 非 background / 有帧 / 非 minimized（toplevel_manager.cpp:11-22） |

### 来源 3：subsurface 层（菜单/popup/GL readback 内容）

| 项 | 事实 |
|----|------|
| 数据 | `SubsurfaceLayer`（desktop_compositor.h:48-61）：pixels/x/y/w/h/localX/localY/shmCommitSerial/parentToplevel/shmFormat/opaque/damage/vpDst/isExternal |
| 生命周期 | 创建：subcompositor（wl_core.cpp:160-177）；位置：set_position（179-222）；重排：place_above/below（224-255）；销毁：surface destroy（103-118）/subsurface_destroy（257-281）/NULL buffer commit（478-499）；内容：commit → `UpsertSubsurfaceLayer`（769-832，**双缓冲轮转**：旧像素归还 sd->pixels，99-101 行注释） |
| 合成规则 | 容器顺序（尾=最顶），在 toplevel 之后统一 blit（576-654）；damage 裁剪 + vpDst clamp + ARGB alpha 混合；**全屏时非 fsWin 的层全部跳过（580 行）**，fsWin 的层走保比例变换（589-601） |
| 输入规则 | 反向遍历置顶命中（140-165）；**ZC key 的层不参与命中（146 行）**；isExternal 命中归 root（152-157）；全屏时 fsWin 的层先命中且同 FitRect 变换（86-105） |
| 位置语义 | `ResolveSubsurfaceLayerPositionLocked`（116-128）：insideWin（offset 在父窗口内容区内）→ 父窗口 compositor 位置 + localX/Y；外部菜单 → wineX/Y + offset（wl_core.cpp:802-819）；minimized 时 -32000 补偿（wl_core.cpp:760-766） |

> **重要发现（对文档 §4.2 的补充）**：subsurface 的合成顺序是**独立容器顺序**，不受 toplevel z-order 约束（576 行循环不按父窗口 z 序）。当前正确性依赖 Wine 的 place_above 调用习惯 + "subsurface 恒在父窗口之上"的 Wayland 语义。Layer 化时若把 Subsurface 挂在父窗口层内，等于**首次引入**"subsurface 随父窗口 z 序"的语义——需确认 Wine 侧没有依赖"子菜单越过更高窗口"的行为（子菜单链当前靠 place_above 维护，wl_core.cpp:224-231 有 P2 风险标注）。

### 来源 4：ZC GL overlay（GPU 纹理）

| 项 | 事实 |
|----|------|
| 数据 | `zeroCopyTexture_`（GL_TEXTURE_EXTERNAL_OES）+ `zeroCopySamplingTransform_`；状态见 §5.3 |
| 更新信号 | `OnZeroCopyFrameAvailable` → `zeroCopyFrameAvailable_` + `zeroCopyFrameSignals_`（66-72） |
| 绘制规则 | 全屏分支（868-885）：保比例缩放进 letterbox 显示区（`ComputeFitRect` 于 layer 尺寸）；非全屏（886-892）：帧内坐标 → surface 视口 `FitMapDisplay*`（Y 翻转） |
| 输入规则 | **不可点击**：ZC key 层不参与命中（146 行），命中交给下方 zOrder 循环 |
| 几何源 | `GetZeroCopyLayerInfo`（130-239）：subsurface 分支从 `SubsurfaceLayer` 取位置 + `pst->fullscreen`；protocol-only 分支（Vulkan private-present）从 `sd->subsurfaceX/Y` 与 `parentState->x/y/wineX` 推算，minimized 也有 -32000 补偿（174-208） |

### 来源 5：遮挡重绘（层序恢复）

| 项 | 事实 |
|----|------|
| 位置 | egl_renderer.cpp:909-948 |
| 执行条件 | `!zeroCopyFullscreen_ && RootCompositing && rendered`（909） |
| 遮挡源计算 | `GetZeroCopyOccluders`（253-309）：zbegin = ZC 父窗口在 zOrder 中的**后一位**（283-287，root 为 ZC 父窗口时从开头）；遍历 zbegin 之后的可见 toplevel 矩形（288-295，全屏窗口 push 整屏）+ 所有**非 ZC** subsurface 矩形（297-307）；每源矩形与 ZC 层矩形求交（pushRect，274-280）；32 上限（912 行注释） |
| 贴回源 | **上一次上传的 CPU 帧纹理**（929-938 注释：桌面帧像素 = CPU 合成最终像素）；UV 换算 `r.x/frameW, r.y/frameH`（纹理第 0 行 = 帧顶，v 无需翻转） |
| 前提假设 | "CPU 帧里遮挡源区域已包含其最终像素"——即遮挡源必须已合成进 CPU 帧且纹理已上传 |

---

## 3. 层序三规则的精确语义

### 规则 A：z-order（合成 480 / 输入 167）

- 唯一存放处 `toplevelZOrder_`（toplevel_manager.h:14 不变式）；Add 到尾部（= 最顶），Remove 即删除；taskbar 常驻顶层（wayland_server.cpp:151-158：raise 时把 taskbar 移到顶，除非被 raise 的是全屏窗口）
- root 可能因识别时序已在列 → `IsToplevelVisibleLocked(root)=false` 兜底（toplevel_manager.h:15-17）

### 规则 B：全屏独占

- **选取**（渲染 393-404 / 输入 54-60，两处独立实现、注释约定同规则）：可见全屏窗口中 fsPriority 最大者
- **fsPriority 取号**：`AddToZOrder` 首次入列（toplevel_manager.h:110-120）；`BumpFsPriorityLocked` 仅用户显式 raise 已全屏窗口（132 行）
- **合成跳过**（487-491）：fsWin 存在时——
  - `isZcGame`（fsWin 有 ZC 层）→ 跳过**全部**其他 toplevel
  - 非 ZC → 跳过其他**被连带标 fullscreen** 的窗口，**非全屏弹窗/对话框保留**
- **fsWin 本身**：ZC → 整帧填不透明黑（500-509，必须 0xFF000000 不能用 memset 0——注释：隐式依赖 GL 不开混合）；SHM → 保比例 `BlitScaled` + 四边黑边填黑（511-534），`fullscreenContentCovered`（fsWin 有全屏覆盖的不透明 subsurface，406-428）时跳过 BlitScaled
- **subsurface 跳过**（580）：全屏时非 fsWin 的层不合成
- **输入**（72-127）：fs-pick 后黑边事件归属 fsWin 但标 swallow（仅吞 PRESS，MOVE/RELEASE 透传——注释：防按键状态卡死，115-126）
- **尺寸选择**（63-71 / `SelectFullscreenContentSize`，geometry.h）：ZC 游戏用 preFs 尺寸，SHM 用 buffer 尺寸

### 规则 C：遮挡重绘（见来源 5）

> 三规则叠加的完整矩阵：**非全屏** = A 决定一切，B、C 不参与（C 只在有 ZC 层时参与）；**SHM 全屏** = B 的"连带全屏跳过" + A 保底；**ZC 全屏** = B 的"全部跳过" + C 被 `!zeroCopyFullscreen_` 禁用——即 ZC 全屏时层序**完全放弃**（fsWin 必须是最顶层，这就是单实例假设）。

---

## 4. 状态机详图

### 4.1 subsurface 生命周期

```
get_subsurface → sd.isSubsurface=true, parentSurface=父
set_position   → sd.subsurfaceX/Y + UpdateSubsurfaceLayerLocalPosition（desktop）
place_above/below → ReorderSubsurfaceLayerAbove/Below（desktop 容器序；PC 模式不映射，P2 标注）
commit         → UpdateSubsurfaceLayerOnCommit（769-832）:
                 opaque 判定（shmFormat!=0 或逐像素查 alpha）
                 insideWin 判定 → layer.x/y（comp 或 wine 系坐标）+ isExternal
                 minimized -32000 补偿
                 UpsertSubsurfaceLayer（双缓冲轮转，旧像素归还 sd->pixels）
                 标记 root dirty
销毁（4 条路径）→ RemoveSubsurfaceLayer；同步清 PC popup 记录
```

### 4.2 全屏状态机

```
SetToplevelFullscreen(on)（wayland_server.cpp:338-361）:
  fullscreen=on；锚定 (0,0)（MW_ASSERT 守卫，358 行）；首快照 preFsW/H（354-355）
  → dirty + compositionSignature 失效（MarkDesktopRootDirtyLocked）
raise（135-160）: Remove+Add zOrder；用户 raise 已全屏窗口 → BumpFsPriority
fs-pick 消费方: 合成（393-404）/ 输入（54-60）/ SurfaceLocalToDesktop warp 逆映射（216-231，同一几何三处）
最小化: SetToplevelMinimized（290-295）；restore 时保持 FULLSCREEN 状态（315-321）
```

### 4.3 ZC 状态机（三处状态 + 单值 attach）

**状态 ① compositor `zeroCopySurfaceKeys_`**（消费方：合成跳过 576-577 / 遮挡跳过 299 / 输入跳过 146 / isZcGame 判定 108-114）
**状态 ② broker `zeroCopyAttachedSurfaces_`**（消费方：Query 返回 attached 标志供 TryAttach 跳过 194 行）
**状态 ③ ready marker 文件**（`WINEHUA_ZERO_COPY_READY_DIR/winehua_zc_surface_<key>.ready`，guest `access()` 判定，opengl_readback.c:121-133）

```
TryAttach（126-281，100ms 节流）:
  未 registered: 遍历 QueryZeroCopySurfaces → 第一个 unattached + vulkan 匹配 + GetZeroCopyLayerInfo 通过
                 → 建 OH_NativeImage/texture/listener → AttachZeroCopyTarget（②入）
                 → zeroCopyRegistered_=true（单值 zeroCopySurfaceKey_）
UpdateZeroCopyFrame 首帧成功（389-399）: SetSurfaceZeroCopy(true)（①入）+ SetZeroCopySurfaceReady(true)（③写）
  → guest 下次 swap 走 ZC present（349 行），否则 glReadPixels readback（SHM 路径）
连续 8 次 Update 失败（309-331）: SetZeroCopySurfaceReady(false)（③删）+ fallbackPending_（记 shm 基线）
  → guest 走 readback；新 SHM commit serial > 基线时（152-165）: SetSurfaceZeroCopy(false)（①出）→ 该层回归普通 subsurface 合成+命中
GPU 恢复（381-388）: fallbackPending_ 取消
Release（428-507）: ③删 → ①出 → ②出（Detach）→ listener unset → image destroy → texture delete → 全状态清零
surface 销毁（wl_core.cpp:111）: RemoveZeroCopyKeyLocked（①出，防悬垂）
```

> **对重构的约束 3**：`zcActive` 单一字段合并三处状态时，必须保留三条独立时序链——①是渲染侧"画什么"（合成/遮挡），②是 guest 侧的 present 选路（**只有 ③ 影响 guest 行为**），①只影响 host 合成。合并成一个 bool 意味着 host 状态变化会**立即**改变 guest 路径（③由 host 写）——当前的 8 次失败缓冲 + shm 基线比对是防抖动设计，合并时不能丢。

### 4.4 双 GL 实例 bug 的机制链（调研推导）

前提：desktop 模式单 renderer + 单值 attach → 两个 GL 实例中**只有一个**能进 ZC，另一个恒走 readback SHM。

1. 实例 A、B 各有一个 GL subsurface（guest 侧 GL surface 挂到 toplevel 下）
2. TryAttach 按 QueryZeroCopySurfaces 顺序选**第一个** unattached——attach 到谁取决于 surface 创建/查询顺序，与 z-order 无必然关系
3. 被 attach 的实例（设 A）画面走 ZC overlay（画在 CPU 帧之上，865-899）；B 走 readback，画面经 SHM 合进 CPU 帧
4. **B 的 GL subsurface 合成顺序不受 toplevel z-order 约束**（576 行循环按 subsurface 容器序）——若 A、B 都是普通窗口，层序正确性完全押在遮挡重绘上
5. 遮挡重绘有 4 个失败点：
   - `zeroCopyFullscreen_=true`（909 行条件）：A_toplevel 一旦被连带标 fullscreen（Wine 按"窗口覆盖整屏"批量标记，toplevel_manager.h:57-62），遮挡重绘**整体跳过** → A 无条件浮最上（B 全屏画面被 A 的 ZC 覆盖）
   - B_toplevel 不可见（`IsToplevelVisibleLocked` 任一条件失败）→ 不产生遮挡源
   - 遮挡源超过 32 矩形 → 超出部分透出
   - CPU 帧纹理未上传（`rendered=false`）→ 整段跳过
6. 反向场景（fs-pick 选中 A，A 是 ZC）：isZcGame=true → 487-491 跳过全部其他 toplevel + 580 跳过非 fsWin 的 subsurface → B 完全不显示。此时层序正确（B 本来就该被盖），但 **B 被整体抹掉**是行为问题（readback 实例在 ZC 全屏下不显示，非"层序"问题）

> 结论：双 GL 实例 bug 的完整修复 = 阶段 2 的"ZC 入层"（层序由 zIndex 排序决定，遮挡重绘删除）+ **解决单值 attach 的公平性问题**（S7：多实例时只有一个一等公民）。文档 §5 阶段 2 只写了前者的两行删除，后者（S7）依赖阶段 3 的 `zcActive` 收敛，但阶段 2 验收场景（双 GL 全屏互叠）必须包含"两个实例都正常显示"的断言。

---

## 5. 回归设施现状

| 设施 | 能力 | 与重构的关系 |
|------|------|-------------|
| `automation/run_regression.py` | `--gate`（3×reuse + 1×clean core）；套件：core/opengl/d3d8/d3d9/venus*/dxvk*；构建 → install → smoke → 固定帧截图校验 → 归档 | **core 套件 = OpenGL smoke 单窗口场景**（`opengl-x64-visual.json` 四象限视觉校验） |
| `validate_frame.py` | PIL/numpy 像素校验（rgba-quadrants-v1-rotations、d3d11-cube-color-depth-v1） | 可复用为阶段 1 像素级回归工具 |
| 设备日志 | `[GL-TAKE]` 合成耗时 / `[MW-TAKE]` 帧输出 / `[MW-SUBSURF]` / `[VIRGL-ZC]` / `[Input] fs-pick` | 双实例/多窗口场景无自动化，需手动脚本 |

> **对重构的约束 4**：现有自动化全部是**单 GL 实例、单窗口内容**场景。阶段 1 验收"合成输出逐像素等价"只能覆盖单实例基线；多窗口/双 GL/全屏互叠场景需要补手工回归清单（列出操作步骤 + 预期 + 日志关键字），这是重构验收的前置条件。

---

## 6. 决策点清单（重构归宿）

每个影响合成输出的条件 → 语义 → Layer 化归宿：

| # | 条件 | 位置 | 语义 | 归宿 |
|---|------|------|------|------|
| D1 | `IsToplevelVisibleLocked(childId, rootId)` | 480 | 可见性 | Layer.visible |
| D2 | `hasFullscreen && childId != fullscreenId` + `isZcGame` | 487-491 | 全屏独占跳过 | zIndex 提升后删除（阶段 2）；被盖层裁剪保留 |
| D3 | `childId == fullscreenId && hasFullscreen` | 499-535 | fsWin 特殊合成（填黑/BlitScaled/黑边） | fsWin 的 zIndex 提升 + SHM 层保比例几何（阶段 2 保留语义） |
| D4 | `layer.parentToplevel != id && !visible(parent)` | 578 | subsurface 可见性 | Layer.visible（父链） |
| D5 | `hasFullscreen && layer.parentToplevel != fullscreenId` | 580 | 全屏 subsurface 跳过 | 同 D2（被 fsWin zIndex 盖住 → 裁剪） |
| D6 | `hasFullscreen && layer.parentToplevel == fullscreenId` | 589-601 | fsWin 的 subsurface 保比例变换 | fsWin 层内 subsurface 的层内 zIndex + 变换语义保留 |
| D7 | `zeroCopySurfaceKeys_.count(layer.surfaceKey)` | 577/299/146/413 | ZC 层不 CPU 合成/不遮挡/不命中/不覆盖判定 | Layer.zcActive + 内容语义（GPU 自绘，CPU 区域留空） |
| D8 | `layer.isExternal` | 152-157 / 121 | 外部菜单输入归 root | Layer 输入归属字段 |
| D9 | `layer.shmFormat == 0 && !layer.opaque` | 599/629 | alpha 混合 | Layer 内容语义（保留） |
| D10 | `!zeroCopyFullscreen_ && RootCompositing && rendered` | 909 | 遮挡重绘执行 | 删除（阶段 2，zIndex 自然覆盖） |
| D11 | `fullscreenContentCovered` | 406-428 | fsWin 被全屏 subsurface 覆盖时跳过 BlitScaled | 阶段 2 后 = fsWin 层内 subsurface 盖住 fsWin 内容 → 裁剪优化（等价保留） |
| D12 | `sd->inputRegionEmpty` | 176 | 空输入区域穿透 | Layer 输入属性 |
| D13 | `cst->fullscreen`（遮挡源 pushRect 整屏） | 293 | 遮挡源几何（全屏窗口 = 整屏） | 阶段 2 后 = 遮挡源几何就是窗口几何（zIndex 规则） |
| D14 | `rendererToplevelId` 匹配（PC 限定） | 211/238 | PC 窗口内 ZC 定位 | 阶段 3 PC Layer 的窗口内几何 |

**行为等价基线（阶段 1 验收用）**：对同一输入序列（commit 事件流），`TakeToplevelFrame` 输出逐像素一致。等价成立的前提：dirty 语义、签名覆盖、out 增量复用、双缓冲轮转 4 个机制在重构后行为不变。

---

## 7. 对 4 个阶段的补充约束清单

| 阶段 | 新增约束（本报告） |
|------|-------------------|
| 阶段 1 | ① Layer 容器沿用 tmgr 锁（§1.1）；② 签名必须覆盖 Layer 列表几何（§1.3）；③ 双缓冲轮转（Upsert 归还旧像素）语义保留；④ 全屏跳过逻辑（D2-D6）等价形式保留，包括 `fullscreenContentCovered`（D11）；⑤ out 增量复用语义保留 |
| 阶段 2 | ① 删除 909 行条件时逐条论证四失败点（§4.4 步骤 5）；② 删除 487-491 后必须补遮挡裁剪（§1.4）；③ 单值 attach（S7）与"两个实例都正常显示"验收的关系（§4.4 结论）；④ ZC 层不可点击语义（D7 输入侧）保留 |
| 阶段 3 | ① `zcActive` 合并时保留 8 次失败缓冲 + shm 基线防抖动（§4.3 约束 3）；② PC 窗口内 subsurface 顺序与系统合成器的关系（wl_core.cpp:224-231 P2 风险）；③ guest 选路只读 ③ 号状态，host 侧合并不得改变 guest 行为时序 |
| 阶段 4 | ① fs-pick 三处消费（合成/输入/warp 逆映射）同源化；② `SelectFullscreenContentSize` 的 ZC/SHM 尺寸语义（preFs vs buffer）保留 |

## 8. 代码地图（调研确认）

| 文件 | 关键位置 |
|------|---------|
| `compositor/desktop_compositor.cpp` | TakeToplevelFrame 315-684；GetZeroCopyLayerInfo 130-239；GetZeroCopyOccluders 253-309；签名 430-464 |
| `egl_renderer.cpp` | RenderLoop 572-961；TryAttach 126-281；UpdateZC 283-426；Release 428-507；overlay 865-899；遮挡 909-948 |
| `wl_core.cpp` | subsurface 生命周期 160-281；toplevel commit 572-704；subsurface commit 769-832；surface 销毁 103-136/370-397 |
| `compositor/input_resolver.cpp` | fs-pick 45-127；subsurface 命中 140-165；toplevel 命中 167-184；warp 逆映射 208-235 |
| `compositor/toplevel_manager.h/.cpp` | ToplevelState 34-79；AddToZOrder 110-120；IsToplevelVisibleLocked cpp 11-22 |
| `graphics_broker.cpp` | Attach 502-518；Query 551-627；Ready marker 629-656；ready 路径 93-97 |
| `thirdparty/wine/.../opengl_readback.c` | 选路 347-369；ready 判定 121-133 |
| `wayland_server.cpp` | Raise 135-160；SetFullscreen 338-361；最小化 290-325 |
| `plugin_manager.cpp` | CreateRenderer 23；MoveRendererToToplevel 102 |
| `compositor/display_policy.h` | 四策略查询点（RootCompositing/SubsurfaceAsLayer/OhosWindowPerToplevel/CompositorRoutesInput） |
