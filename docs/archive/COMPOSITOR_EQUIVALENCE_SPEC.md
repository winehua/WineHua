# 合成重构阶段 1：行为等价规范

> 状态：已完成（2026-09-22 归档）。合成器重构依照本文的等价定义实施完毕并合入 master。本文仅作历史记录。

> 状态: 规范（2026-08-01）
> 依据: `COMPOSITOR_UNIFICATION.md` §5 阶段 1 + `COMPOSITOR_REFACTOR_RESEARCH.md` §6/§7
> 目的: 把"行为等价重构"从口号变成**可验证的定义**：状态怎么映射、条件怎么保持、
>       怎么证明等价、验收什么
> 范围: desktop 模式合成（`TakeToplevelFrame`）+ 输入命中（`FindInputTargetAt`）。
>       **不含** egl_renderer GPU 侧（overlay/遮挡重绘）、不含 PC 模式、不含 ZC 状态机。

---

## 1. 等价定义（4 层）

| 层 | 定义 | 验证手段 |
|----|------|---------|
| L1 合成输出 | 对同一逻辑状态序列，`TakeToplevelFrame(rootId)` 输出 **w/h + 像素逐字节一致**（无容差） | 在线双跑 assert（§9.2） |
| L2 输入命中 | 对同一状态 + 同一 (x,y) 序列，`FindInputTargetAt` 返回**全字段一致**：toplevelId / surface / originX / originY / scale / swallow | 在线双跑 assert（§9.2） |
| L3 状态映射 | Layer 容器 = 旧容器（`toplevelZOrder_` + `subsurfaceLayers_` + `zeroCopySurfaceKeys_`）的**纯函数投影**，逐字段见 §3 | 静态审计（§9.1） |
| L4 触发时机 | dirty 置位时机、compositionSignature 变化时机（= rebuildBase 判定）**逐帧一致**，即"哪一帧重新合成、哪一帧增量叠加"完全一致 | 双跑帧数对比 + 签名覆盖审计（§9.2/§8） |

**等价的意义**：L3 是结构性保证（映射对了，行为就不会差），L1/L2 是观测性保证（真跑起来不差），L4 是性能/时序保证（重建基底帧序不变，避免合成输出出现"晚一帧"的差异）。

**明确排除（阶段 1 不得改变的行为）**：
- 双 GL 实例 bug **必须原样保留**（阶段 2 修复）——等价验收遇到该 bug 是"符合预期"
- 遮挡重绘、ZC overlay 绘制、PC 模式、ZC fallback 节奏——一律不动
- 日志、性能（`[GL-TAKE]` 可劣化在测量阈值内，§7.1）

---

## 2. 等价的前提：输入序列可复现性

等价对比（双跑/快照）要求**同一逻辑状态**。合成器的输入是 Wayland 协议事件流，等价对比时两类输入的区别：

| 输入类型 | 处理方式 |
|---------|---------|
| **确定性状态**：窗口几何、zOrder、subsurface 容器序、全屏标志、visible、ZC key 集合 | 双跑直接对比（这些是纯状态，L1/L2 对比的就是它们） |
| **时序性状态**：commit serial、dirty 位、frame callback 节奏 | 双跑**不重置**，随事件流自然演变——等价要求的是"对同一演变序列"输出一致，不是"对同一时刻" |

> 推论：双跑对比必须在**同一次事件流**里做（新/旧路径消费同一份状态，产出各自输出再比较），而不是"先后跑两次"。§9.2 的在线双跑就是为此设计。

---

## 3. Layer 状态映射表（L3 核心）

### 3.1 容器投影

```
Layer 列表 = [ 每个 toplevel 一个 Layer(Toplevel) ] 按 toplevelZOrder_ 顺序
          + [ 每个 subsurface 一个 Layer(Subsurface) ] 按 subsurfaceLayers_ 容器顺序
          + [ 每个 ZC key 一个 Layer(ZeroCopy) ]       按 zeroCopySurfaceKeys_ 遍历序
```

- **单一容器**：`std::vector<CompositorLayer>`，替代 `subsurfaceLayers_` 与零散遍历；`toplevelZOrder_` 保留（输入 raise/move 等**非合成**路径仍用它，见 §3.3）
- **同步点**：所有旧容器写入点（wl_core.cpp commit/销毁、wayland_server.cpp raise/fullscreen、desktop_compositor.cpp SetSurfaceZeroCopy 等）同步维护 Layer 列表——阶段 1 旧容器**仍是权威**，Layer 是镜像（§9.1 审计的就是"每个写入点都同步了"）

### 3.2 字段映射（逐字段）

**ToplevelState → Layer(Toplevel)**

| 旧字段 | Layer 字段 | 转换规则 |
|--------|-----------|---------|
| `x/y/w/h` | `x/y/w/h` | 直接拷贝 |
| `fullscreen` | `fullscreen` | 直接拷贝 |
| `preFsW/H` | （不入 Layer，输入侧 fs-pick 仍需，保留在 ToplevelState） | — |
| `shmFormat` | `shmFormat` | 直接拷贝 |
| `pixels` | `pixels` | **引用不拷贝**（与 `rst->pixels` 共享，合成读） |
| `dirty` | `dirty` | 直接拷贝语义（§8 触发机制） |
| 可见性 | `visible` | = `IsToplevelVisibleLocked(id, rootId)`（**派生不存储**，与旧逻辑一致——旧代码每帧现算，Layer 若缓存必须在每次读时刷新，建议仍现算） |
| — | `zIndex` | §4 分配规则 |

**SubsurfaceLayer → Layer(Subsurface)**

| 旧字段 | Layer 字段 | 转换规则 |
|--------|-----------|---------|
| `surface` | `surface` | **必须保留 wl_resource* 指针**（旧签名 mix 了 `reinterpret_cast<uintptr_t>(layer.surface)`，desktop_compositor.cpp:454；换成 surfaceKey 会改变签名 → 破坏 L4） |
| `surfaceKey` | `surfaceKey` | 直接拷贝 |
| `pixels/x/y/w/h/localX/localY/shmCommitSerial/parentToplevel/shmFormat/opaque/damage/vpDstW/vpDstH/isExternal` | 同名 | 直接拷贝 |
| 是否 ZC | `zcActive` | = `zeroCopySurfaceKeys_.count(surfaceKey) != 0`（**派生**，随 key 集合变） |
| — | `zIndex` | §4 分配规则（必须保持"subsurface 段在 toplevel 段之后"） |

**ZC key → Layer(ZeroCopy)**

| 旧 | Layer | 转换规则 |
|----|-------|---------|
| key | `surfaceKey` | 直接 |
| — | `type=ZeroCopy`, `zcActive=true` | 阶段 1 该层**只做占位**：CPU 合成跳过（D7 等价）、不产生像素；overlay/遮挡逻辑阶段 1 不读 Layer（仍在 egl_renderer） |

### 3.3 不并入 Layer 的旧状态（保持原样）

| 状态 | 原因 |
|------|------|
| `toplevelZOrder_` | raise/move/taskbar 逻辑（wayland_server.cpp:135-160）仍按 zOrder 操作，改它们的成本与收益不成比例；Layer 只服务"合成 + 输入遍历" |
| `ToplevelState` 其余字段（wineX/wineY/hasPosition/fsPriority/mask/minimized/isBackground） | 输入 fs-pick、遮挡计算等仍直接用；minimized/isBackground 是 visible 的输入，不入 Layer |
| `zeroCopySurfaceKeys_` 本身 | 它是 ZC 状态机的一部分（阶段 3 才合并）；阶段 1 Layer.zcActive 派生自它 |

---

## 4. zIndex 分配规则（顺序保持证明）

```
kTopCount = 当前 toplevel Layer 数
Layer(Toplevel,   zIndex = zOrder 中的位置)          // 0..kTopCount-1
Layer(Subsurface, zIndex = kTopCount + 容器位置)     // kTopCount..（含 ZC 层，见下）
Layer(ZeroCopy,   zIndex = kTopCount + 容器位置)     // 与其所属 subsurface 同位（zcActive 标记区分）
```

**证明（合成 blit 顺序不变）**：
- 旧：toplevel 按 zOrder 升序全画 → subsurface 按容器序全画（subsurface 恒在 toplevel 之上，desktop_compositor.cpp:480-573 → 576-654）
- 新：CPU 合成循环按 zIndex 升序遍历、跳过 `zcActive` 层（D7 等价）
- 两段拼接的 zIndex 分配保证遍历序列 = 旧序列，且 `zcActive` 跳过 = 旧 `zeroCopySurfaceKeys_.count` 跳过（576-577 行）

**层序规则（B：全屏跳过）在阶段 1 的等价形式**：
- 循环体保留全部条件判断（D2/D3/D5/D6 原样），只是把"遍历 `toplevelZOrder_` + 遍历 `subsurfaceLayers_`"合并为"遍历 Layer 列表 + 按 type 分支"
- **禁止**在阶段 1 用 zIndex 提升表达全屏（那是阶段 2 的语义变化）

**签名保持（L4 关键）**：新签名 mix 项 = 旧 14 项逐一对应（desktop_compositor.cpp:430-464）：
| 旧 mix 项 | 新 mix 项 |
|-----------|----------|
| root id / rootW / rootH | 不变 |
| 每 zOrder 成员 id / visible / x / y / w / h / fullscreen | 遍历 Layer(Toplevel) 子集，同字段 |
| 每 subsurface 的 surface 指针 / 是否 ZC / parentToplevel / parent 可见性 / x / y / w / h / vpDstW / vpDstH | 遍历 Layer(Subsurface) 子集，同字段（surface 指针保留，见 §3.2） |

> 等价检验：对同一状态，新旧签名计算值必须相同（双跑 assert 之一）。

---

## 5. 决策点归宿规范（D1-D14 具体化）

图例：`[等价保留]` = 阶段 1 必须保持的行为；`[阶段2变化]` = 阶段 1 禁止引入，留待阶段 2。

| # | 条件（旧） | 阶段 1 等价形式 | 验证 |
|---|-----------|----------------|------|
| D1 | `IsToplevelVisibleLocked(childId, rootId)` | 遍历时对 Layer(Toplevel) 现算（同函数），**不缓存** | 双跑 |
| D2 | `hasFullscreen && childId != fullscreenId` + `isZcGame` | 保留：循环体内按 type 分支，Toplevel 子集上原条件 `[等价保留]`；阶段 2 才删除 `[阶段2变化]` | 双跑 |
| D3 | fsWin 特殊合成（填黑/BlitScaled/黑边） | 保留（Toplevel 子集上原逻辑，含 `0xFF000000` 填黑注释语义） | 双跑 |
| D4 | `layer.parentToplevel != id && !visible(parent)` | 保留：Subsurface 子集上原条件 | 双跑 |
| D5 | `hasFullscreen && layer.parentToplevel != fullscreenId`（580） | 保留 | 双跑 |
| D6 | fsWin 的 subsurface 保比例变换（589-601） | 保留（用 `transform` + `lround(scale)` 原式） | 双跑 |
| D7 | `zeroCopySurfaceKeys_.count(layer.surfaceKey)`（577/299/146/413） | 统一为 `layer.zcActive`（派生值），四处消费点同一来源 | 双跑 + 静态审计 |
| D8 | `layer.isExternal` | 字段直移 | 双跑 |
| D9 | `shmFormat == 0 && !opaque` | 字段直移 | 双跑 |
| D10 | 遮挡重绘（909-948） | **整体不动**（egl_renderer 不在本阶段范围）`[阶段2变化]` | 回归 |
| D11 | `fullscreenContentCovered`（406-428） | 保留（Subsurface 子集扫描，原逻辑） | 双跑 |
| D12 | `sd->inputRegionEmpty` | 输入循环内保留（来自 surface 的 SurfaceData，与 Layer 无关） | 双跑 |
| D13 | 遮挡源 pushRect（293） | 不动（D10 同） | 回归 |
| D14 | PC 限定（211/238） | 不在本阶段范围 | — |

**输入侧三个消费点**（L2）：
- fs-pick 扫描：从 `tmgr_.toplevelZOrder()` 改为遍历 Layer(Toplevel) 子集——**fsPriority 比较逻辑原样**（input_resolver.cpp:54-60）
- subsurface 命中：`subsurfaceLayers().rbegin()` 改为 Layer(Subsurface) 子集反向——**isExternal / ZC skip / 位置解析原样**（140-165）
- toplevel 命中：`toplevelZOrder().rbegin()` 改为 Layer(Toplevel) 子集反向——**inputRegionEmpty 检查原样**（167-184）
- 黑边 swallow、`SelectFullscreenContentSize`、`ComputeFitRect` 全部原样

---

## 6. 几何与取整规则（L1 逐字节一致的硬约束）

**规则 1**：新代码不得引入任何新的几何计算。所有变换必须调用 geometry.h 的同一函数：
- 合成：`ComputeFitRect`、`BlitScaled`（现有），取整用 `lround(scale)` 原式（589-595）
- 输入：`FitMapX/Y`、`FitUnmapX/Y`（未取整 scale 逆映射）、`SelectFullscreenContentSize`
- 禁止：任何 `static_cast<int>` 取整替代 `lround`；任何"看起来等价"的重写（历史上 1px 偏差即此类，egl_renderer.cpp:811-813 注释）

**规则 2**：遍历顺序 = 输出顺序。Layer 列表的 zIndex 排序必须是**稳定的**（`std::stable_sort` 或插入保持），重复 zIndex 不得发生（§4 分配保证无重复）。

**规则 3**：像素操作（memcpy / alpha 混合 / BlitScaled）**原样搬移**，只改数据来源（`rst->pixels` → `layer.pixels` 引用、`subsurfaceLayers_` → Layer 子集），不改算法。

---

## 7. 性能等价（次要目标，不阻塞）

- `[GL-TAKE]` 6 段耗时在重构前后各采集 120 样本：lockWait/rootCopy/children/subsurfaces/total 的 avg 劣化 **≤ 15%**（Layer 遍历多一层间接的合理上限）
- rebuildBase 帧数（L4 一致）必须相同——**这是结构保证**（签名覆盖审计），若不同即 L4 失败

---

## 8. 触发机制等价（L4）

| 机制 | 等价要求 |
|------|---------|
| dirty 置位点 | 全部保持（commit/raise/fullscreen/subsurface 操作/SetSurfaceZeroCopy 各点调用 `MarkToplevelDirtyLocked`/`MarkDesktopRootDirtyLocked` 不动） |
| 签名计算 | §4 表格逐项对应，双跑 assert 相同 |
| root serial | `IncrementDesktopRootFrameSerial` 不变（wl_core.cpp:583-585） |
| 双缓冲轮转 | `UpsertSubsurfaceLayer` 的"旧像素归还 sd->pixels"语义在 Layer 容器里等价实现（同指针复用，不得多拷一份） |
| out 增量复用 | `TakeToplevelFrame` 的 out buffer 复用语义不变（rebuildBase=false 时在现有 out 上叠加）——Layer 化后 out 的持有者仍是渲染线程 |

---

## 9. 验证方法

### 9.1 静态审计（L3，代码 review 清单）

1. 旧容器每个写入点 → Layer 镜像同步点，一一对应（grep 旧容器写操作，对照 §3.3 排除表）
2. 映射表字段逐一核对（§3.2）
3. 决策点归宿核对（§5）：`[等价保留]` 条件全部在循环体内，`[阶段2变化]` 全部未引入
4. 签名 mix 项与 §4 表格逐项相同
5. 几何函数调用审计（§6 规则 1：grep 新代码无新 `lround`/`static_cast<int>` 之外的取整）

### 9.2 在线双跑（L1/L2/L4 的最终判定）

**机制**：环境变量 `WINEHUA_EQUIV_CHECK=1` 启用。`TakeToplevelFrame` / `FindInputTargetAt` 内：
- 新旧两条路径同时计算（阶段 1 把旧循环提取为 `ComposeLegacy()` / `ResolveLegacy()` 保留在 `#ifdef` 或独立函数）
- 逐字节 assert：像素（含尺寸）、InputTarget 全字段、签名值
- 不一致 → 打印差异首位置（第一个不同像素的 (x,y,通道,新旧值)）并走旧路径输出（**fail-safe：验证期故障不黑屏**）
- 一致 → 输出新路径结果
- 验证期结束：删旧路径 + EQUIV_CHECK 开关

**验收数据**：
- 双跑 assert 在整轮手工场景 + automation gate 中 **0 次不匹配**
- rebuildBase 触发帧数新旧一致（日志对比）

### 9.3 自动化套件（现有）

`python3 automation/run_regression.py --gate`（3×reuse + 1×clean core，OpenGL smoke 单实例基线）——通过且**带 EQUIV_CHECK 跑一遍**。

### 9.4 手工等价场景清单（L1/L2 全覆盖）

> 每个场景：操作步骤 + 观察点（视觉/日志）。全部通过 = 阶段 1 验收。

| # | 场景 | 操作 | 观察点 |
|---|------|------|--------|
| E1 | 单窗口生命周期 | explorer 桌面 + 开 notepad：移动/缩放/resize/最小化/恢复/关闭 | 视觉无残影；`[MW-TAKE]` 尺寸正确 |
| E2 | 多窗口叠层 | 3 窗口错叠，点击 raise、任务栏 raise | 叠序视觉正确；`[Input] fs-pick` 无意外输出 |
| E3 | subsurface 菜单 | 窗口内右键菜单、子菜单链、任务栏右键（isExternal）、菜单拖出父窗口边界 | 菜单可点；命中菜单项生效（无 clamp 夹回） |
| E4 | subsurface 生命周期 | 菜单开/关、NULL buffer commit、place_above/below 重排 | 无残留像素；`[MW-SUBSURF]` 日志 |
| E5 | SHM 全屏 | 红警2 类：进/出全屏、黑边点击、resize | 全屏画面 + 黑边 swallow 行为（第一下点击不切前台） |
| E6 | ZC 全屏 | PAL2 类：进全屏、游戏内点击/移动 | `[VIRGL-ZC]` GPU_ACTIVE；点击命中游戏 |
| E7 | ZC fallback | 触发 8 次连续失败（如 GPU 忙）→ fallback → 恢复 | `CPU_FALLBACK` 日志；画面转 SHM 再转回无黑屏 |
| E8 | 双 GL 实例（bug 基线） | 两个 GL 程序实例互叠 | **行为与重构前一致**（bug 仍在 = 通过；行为变化 = 失败） |
| E9 | root 重建 | 杀 explorer → 重启 | 桌面恢复；`MoveRendererToToplevel` 日志；无"仅剩背景" |
| E10 | 任务栏 | 全屏窗口压过任务栏 / 窗口化时任务栏置顶 | 两种模式下任务栏层序正确 |
| E11 | 静态桌面功耗 | 无新帧场景静置 10s | 无持续重绘（无新 `[MW-TAKE]` 输出） |

---

## 10. 阶段 1 实施顺序（每步一个可验证里程碑）

```
M1  建 Layer 容器 + 全同步点（只写不读）
    → 审计 9.1.1/9.1.2；旧容器权威不变，双容器并存
M2  TakeToplevelFrame 改 Layer 遍历（含签名改 Layer 版本）+ EQUIV_CHECK 双跑
    → 跑 E1-E11 + --gate，assert 0 不匹配
M3  FindInputTargetAt 改 Layer 遍历 + EQUIV_CHECK 双跑
    → 同 M2
M4  删旧容器（subsurfaceLayers_/zeroCopySurfaceKeys_ 移除或降级为只读视图）
    → 全场景复跑 + --gate（不带 EQUIV_CHECK）
M5  清 EQUIV_CHECK 代码 + 性能对比（[GL-TAKE] 120 样本 ≤15% 劣化）
    → 收尾，阶段 2 开工
```

**M1 的特别要求**：Layer 镜像的同步必须与旧容器**同一锁内**（§调研报告 1.1 约束 1），防止镜像漂移。

---

## 11. 验收清单（勾选表）

- [ ] §9.1 静态审计 5 项全部通过（含签名 mix 项逐一核对）
- [ ] EQUIV_CHECK 双跑：E1-E11 + `--gate` 全程 **0 次不匹配**
- [ ] L4：rebuildBase 帧数新旧一致（日志对比）
- [ ] 双缓冲轮转语义保持（无额外拷贝）
- [ ] `[GL-TAKE]` 性能劣化 ≤ 15%
- [ ] 双 GL 实例 bug 行为**不变**（E8 通过即证明等价，不是修复）
- [ ] 代码无 `[阶段2变化]` 项提前引入（§5 表格复查）

---

## 12. 防"顺手改"清单（阶段 1 禁止项）

| 禁止 | 原因 |
|------|------|
| 用 zIndex 提升表达全屏独占 | 阶段 2 语义变化 |
| 删除遮挡重绘 / 改 `!zeroCopyFullscreen_` 条件 | 阶段 2 |
| 改 ZC 三处状态的同步时序（8 次失败缓冲、shm 基线） | 阶段 3 |
| 动 PC 模式（plugin_manager/非 root 分支） | 阶段 3 |
| "顺手"修双 GL bug | 等价验收以 bug 保留为通过标准，修复必须走阶段 2 并在等价对比后独立验证 |
| 引入新的取整/几何计算 | §6 规则 1 |
| 改变签名 mix 的 key（surface 指针 → surfaceKey） | L4 破坏（§3.2） |
