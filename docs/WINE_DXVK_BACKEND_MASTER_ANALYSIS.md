# master DXVK 档位审计：dxvk 1.10 / 2.6 逻辑中的低帧率问题

> 基准位点：master `d6c9575`（含 submodule：dxvk 5058927 / dxvk-modern 977a3d78 / vkd3d-proton 3e5aab6f）
> 配套文档：`docs/WINE_FPS_DIFFERENTIAL_ANALYSIS.md`（四样本判定矩阵与机理）

---

## 0. 结论（TL;DR）

**master 的档位逻辑存在造成低帧率的问题，且就位于出厂默认路径：**

1. 未设置任何偏好时，master 默认组合 = **`vkd3d_limited_500k`（渲染策略，机制组无条件全开）** × **`dxvk_legacy`（DXVK 1.10.3）** —— 正是四样本判定的**低帧率组合**（样本 B 62819 / C 33509 同构造）。
2. 自动选 2.6 只发生在 **VYG-AL00 + incrementalVersion `26.0.0.32` 白名单**（`EntryAbility.ets:10-19`）。实测本机 **SLG-W10 / tablet / 无 incrementalVersion → 白名单不命中 → `dxvk_legacy`**。即：非白名单设备出厂即低帧率，高帧率（样本 A）只是"用户显式选过 2.6"的路径。
3. **次级通道（master 新增）**：stable overlay（`env_profiles.cpp`，1.0.12 无此文件）给**任意 DXVK 会话**（desktop 链 + runWineProgram 直启链）补 `VN_WINEHUA_STRONG_RING_BARRIER=1` 与 `DXVK_WINEHUA_PRECISE_SHADOW=1`（`env_profiles.cpp:130-134`）。因此"纯 dxvk 路由 × legacy = 高帧率"（1.0.12 的 D 组合）**在 master 上被打了折扣**——legacy 在桌面链仍吃到部分机制组键（强环 + precise shadow），是否复现低帧率**待实测**（§7 验证 1）。

---

## 1. 档位模型（两个独立维度）

| 维度 | 取值 | 默认（无持久化） | 持久化键 | 生效值键 |
|---|---|---|---|---|
| 渲染策略 `d3dBackend` | `vkd3d_limited_500k` / `dxvk_modern_2_6` / `dxvk_legacy` / `wined3d` | **`vkd3d_limited_500k`** | `winehua.d3d.preference` | `winehua.d3d.backend` |
| DXVK 档位 `dxvkBackend` | `dxvk_modern_2_6` / `dxvk_legacy` | **`auto` → `defaultDxvkBackend()`**（白名单见下） | `winehua.dxvk.preference` | `winehua.dxvk.backend` |

默认值三处独立声明（必须保持一致，否则出现"默认不一致"的脏值注入）：

| 位置 | 内容 |
|---|---|
| `EntryAbility.ets:32-38,46` | `persistedD3d || 'vkd3d_limited_500k'`；`dxvk.preference || 'auto'` → `resolveDxvkBackend` → 白名单 |
| `WineEnvService.ets:310-313` | 同上默认（`updateLaunchRequest`） |
| `Index.ets:20-22,52-53` | `@StorageLink` 与本地成员默认 `vkd3d_limited_500k` / `dxvk_legacy` |

**白名单（唯一自动 2.6 的门）**：`EntryAbility.ets:10-19`

```ts
const qualified920 = deviceInfo.productModel === 'VYG-AL00' &&
  deviceInfo.incrementalVersion === '26.0.0.32';
return qualified920 ? 'dxvk_modern_2_6' : 'dxvk_legacy';
```

注释自述设计意图："evidence allowlist, not Host-to-Guest capability inference"——HAP 打包的 Guest Mesa/DXVK 载荷固定，只有审计过该 Host 驱动身份的机型才自动上 2.6，系统更新后身份变化即回落 1.10.3。**结论：这不是能力探测，是设备白名单；白名单外设备在渲染策略=vkd3d 路由时自动落入低帧率组合。**

---

## 2. native 注入矩阵（`wine_env.cpp` + `env_profiles.cpp`）

### 2.1 注入函数与行号

| 函数 | 位置 | 走哪些会话 |
|---|---|---|
| `AppendD3dBackendEnv` vkd3d 分支 | `wine_env.cpp:145-255` | 所有 vkd3d_limited_500k 会话 |
| `AppendD3dBackendEnv` dxvk 分支 | `wine_env.cpp:256-362` | 所有 dxvk_* 会话 |
| `AppendStableDxvkEnv` | `env_profiles.cpp:55-142` | desktop 链（`wine_launch.cpp:608` explorer）+ 直启链（`wine_exe.cpp:218` runWineProgram），`usesDxvkOverlay` 时进入 |

### 2.2 机制组键 × 注入来源（`✓` 注入 / `—` 无）

| 键 | vkd3d 分支 | dxvk 分支 | stable overlay | 判别意义 |
|---|---|---|---|---|
| `VN_WINEHUA_STRONG_RING_BARRIER=1` | ✓ :224 | — | **✓ probe 值，缺省 `1`** :133-134 | 强环机制 |
| `VN_WINEHUA_PERSISTENT_MAP_SYNC=1` | ✓ :226 | — | — | 影子内存 |
| `VN_WINEHUA_DIRECT_FENCE_WAIT=1` | ✓ :227 | — | — | 直接 fence |
| `VKR_WINEHUA_SHADOW_FROM_HOST=precise` | ✓ :228 | — | — | 影子契约 |
| `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1` | ✓ :229 | — | — | 一致性映射 |
| `VN_WINEHUA_REMOTE_MEMORY_SYNC=1` | ✓ :225 | ✓ :338 | — | 共享环传输 |
| `WINEHUA_PERF_PROFILE=shadow-precise` | ✓ 写死 :181 | — | ✓ probe 值，缺省 `shadow-precise-dirty-ring-inline-upload-coverage-sort` :111-118,129 | 契约档标签 |
| `DXVK_WINEHUA_PRECISE_SHADOW=1` | — | — | **✓ 无条件** :130 | DXVK 侧 precise 影子 |
| legacy 五件套 (`DXVK_WINEHUA_*`+`WINEHUA_DXVK_RELAXED_FEATURES`) | ✓ legacy 时 :243-253 | ✓ legacy 时 :349-361 | — | 1.10.3 兼容 |

**读表结论**：
- vkd3d 路由下机制组**全部无条件注入**，DXVK 档位只改变 overlay 目录 + VN_PERF + 五件套；
- 纯 dxvk 路由下一丝机制组没有（仅 `REMOTE_MEMORY_SYNC`）——这是 D 样本高帧率的代码面；
- **stable overlay 是 master 新增的"机制组渗漏层"**：`STRONG_RING_BARRIER` 在 probe 里没值时**补 `1`**（不是 `0`）——对 legacy 会话同样补。这正是"文件管理双击（explorer 链）低帧率"的走线结构（样本 C）。

### 2.3 组合判定表

| 组合 | 机制组 | DXVK | 帧率 | 依据 |
|---|---|---|---|---|
| A | 完整（vkd3d 分支 6 键） | 2.6.2 | **高** | 样本 32079 |
| B/C | 完整（同上） | 1.10.3 | **低** | 样本 62819 / 33509 |
| D | 无（1.0.12 纯 dxvk 路由，无 stable overlay） | 1.10.3 | **高** | 样本 53199 |
| E | 部分（stable overlay 2 键：强环+precise shadow） | 1.10.3 | **?? 待测** | master 特有路径，1.0.12 无此通道 |

> master 与 90edaae 在机制组注入键上无差异（`git diff 90edaae master -- wine_env.cpp` 机制组键行为零变化），§2.3 中 A/B/C 结论可直接映射 master。

---

## 3. 低帧率组合在 master 的全部可达路径

| # | 路径 | 组合 | 帧率 |
|---|---|---|---|
| 1 | **出厂默认**（无持久化，非白名单设备）：渲染策略卡=vkd3d_limited_500k（默认）+ DXVK 档=auto→legacy | 机制组完整 × 1.10.3 | **低** |
| 2 | 设置页「渲染策略=vkd3d（默认卡位）」+「DXVK 档=legacy（默认卡位）」——交互上就是不加改动直接运行 | 同上 | **低** |
| 3 | 用户为兼容老游戏显式把 DXVK 档切回 1.10.3（渲染策略保持 vkd3d） | 同上 | **低**（UI 无警示） |
| 4 | 文件管理 / explorer 双击启动：会话 env 继承（`wine_launch.cpp:608` explorerPolicy，继承档位默认；`WineEnvService` 会话链同理） | 同上 | **低**（样本 C 实证） |
| 5 | 纯 dxvk_legacy 路由（渲染策略=dxvk_legacy）+ desktop/explorer 链 | 部分（stable 2 键）× 1.10.3 | **待测**（§7-1） |

高帧率路径（如实列出）：白名单设备自动 2.6；任一设置选 `dxvk_modern_2_6`；渲染策略=纯 dxvk 路由 + 独立窗口（无 stable overlay 时，同 D）。

---

## 4. 机理简述（为什么 1.10 必须配"无机制组"）

1.10.3（经典上传模型：map/unmap + 隐式同步 + host 轮询 fence）与 2.6.2（precise shadow 契约：持久映射同步、direct fence wait、强环 barrier、coherent map sync）是不兼容的两代同步模型。机制组键是 2.6.2/vkd3d-proton 侧的协作契约；1.10.3 对这套键不协作（忽略或回退成每帧 host 同步），最终表现为提交率被钉在低帧率（详见 `WINE_FPS_DIFFERENTIAL_ANALYSIS.md` §7 机理推断）。

## 5. 修复候选（按优先级）

1. **vkd3d 路由默认 DXVK 档改 `dxvk_modern_2_6`**：把"出厂默认=低帧率"换成"出厂默认=高帧率"，1.10.3 保留为显式兼容档（老游戏回退）。白名单逻辑反向——低帧率档才需要白名单"逐机型放行"，而不是高帧率档需要。
2. **UI 组合警示**：渲染策略=vkd3d + DXVK 档=1.10.3 时，设置页提示"低帧率组合，建议 2.6.2"（对齐 §3-3）。
3. **stable overlay 按键归属修正**：`STRONG_RING_BARRIER`/`PRECISE_SHADOW` 是 2.6 契约键，probe 缺省时不应补 `1`（应 `0`/不补），尤其对 legacy 会话——消除 §3-5 渗漏（须先实测 E 是否真慢）。
4. **白名单外设备的默认路由重排**：若不能全量上 2.6（未审计驱动风险），可让非白名单设备默认渲染策略落到纯 `dxvk_legacy` 路由（D 组合高帧率；但失去 D3D12 支持，且对设备清单需要产品决策）。

## 6. 与 1.0.12 位点的结构差异（为何 D 高、master 默认低）

| 结构 | 1.0.12 (68f90d1) | master (d6c9575) |
|---|---|---|
| vkd3d 混合路由 | 无（纯 dxvk 时代） | 默认渲染策略 |
| 机制组注入 | 无 | vkd3d 分支全开 |
| stable overlay | **文件不存在**（`git show 68f90d1:.../env_profiles.cpp` MISSING） | desktop+直启链均过 |
| DXVK 档默认 | `dxvk_legacy`（与渲染地锚成对） | `auto` → 白名单（本机 SLG-W10 → legacy） |

## 7. 证据缺口 / 待验证

1. **§2.3-E 实测**：master 设备设置 渲染策略=dxvk_legacy（纯路由）+ 桌面模式 + cube，观察帧率与 `proc_snapshot.log` 中是否有 `VN_WINEHUA_STRONG_RING_BARRIER`/`DXVK_WINEHUA_PRECISE_SHADOW`。若低 → stable overlay 渗漏成立，修复候选 3 优先；若高 → 渗漏键对 1.10.3 无害，仅保留候选 1/2/4。
2. master 设备默认档验证：清资料（`winehua.dxvk.preference`）后启动，确认快照 = `WINEHUA_D3D_BACKEND=vkd3d_limited_500k` + `WINEHUA_DXVK_VERSION=1.10.3`（即出厂默认组合复现）。
3. 白名单外盘点：1.4 设备（192.168.1.4:44959 在线）的 `const.product.model`（大概率同样不命中）。

## 附录 A：关键源码行号速查

| 代码 | 位置 | 语义 |
|---|---|---|
| `defaultDxvkBackend` | `EntryAbility.ets:10-19` | 白名单 → modern/legacy |
| `publishLaunchRequest` | `EntryAbility.ets:26-42` | 默认值发布到 AppStorage |
| `setD3dPreference` | `WineEnvService.ets:278-292` | 渲染策略切换；选 dxvk_* 时联动 DXVK 档 |
| `setDxvkPreference` | `WineEnvService.ets:294-307` | DXVK 档切换；d3d 为纯路由时联动渲染策略（vkd3d 路由不动） |
| `updateLaunchRequest` | `WineEnvService.ets:309-317` | 生效值回读，默认 `vkd3d_limited_500k`/`dxvk_legacy` |
| `AppendD3dBackendEnv` vkd3d 分支 | `wine_env.cpp:145-255` | 机制组全开 :224-229；legacy 五件套 :243-253 |
| `AppendD3dBackendEnv` dxvk 分支 | `wine_env.cpp:256-362` | 无机制组；legacy 五件套 :349-361 |
| `AppendStableDxvkEnv` | `env_profiles.cpp:55-142` | strong ring :133-134 缺省补 `1`；precise shadow :130 |
| stable overlay 启用点 | `wine_exe.cpp:218` / `wine_launch.cpp:608` | runWineProgram 直启链 / explorer 桌面链 |
| 设备白名单实况 | `param get const.product.model` | SLG-W10 → 不命中 |

---

## 7. 设备实测终局（2026-08-30，1.8 pad SLG-W10，master 上直接验证）

| 轮次 | 配置 | 注入键 | 帧率 |
|---|---|---|---|
| Case 1 | vkd3d_500k × 1.10.3（出厂默认，机制组全开 + no_multi_ring） | 全机制组 | 低 |
| E | 纯路由 × 1.10.3（stable overlay 补强环/precise/PERF_PROFILE） | partial 机制组 | 低 |
| F | 纯路由 × 1.10.3（env 与 1.0.12 D 样本**逐一比对一致**：无机制键 + VN_PERF 短版无 no_multi_ring） | **无** | **低** |
| 2.6.2 | 纯路由 × 2.6.2 | stable overlay 键 | **高** |

**修订 §0 / §1 的重要结论：**

1. 低帧率的充要条件 = **master 的 libentry.so（90edaae→d6c9575 间 entry 层代码）× dxvk 1.10.3**。机制组 / legacy 五件套 / VN_PERF no_multi_ring / 启动路径 **均非必要条件**——F 组合把全部候选键剥离干净，帧率仍低。
2. 1.0.12 的 D 样本（高帧率）是因为 main-ui 旧 entry 层管线与 1.10.3 相容——**wine/dxvk/box64/virgl 二进制全是同一批 90edaae 构建产物**（1.0.12 重打包时未重建任何 native 库），唯一变量是 libentry.so 层与 ArkTS。
3. **因此 master 上 1.10.3 不存在"高帧率配置"**；它是与 master 帧管线（零拷贝帧传输 / virpipe / venus broker present）硬不兼容的能力回退档。之前差分文档的"修复候选①②（机制组减配/免开关）"已被证伪，应删除。
4. 造成用户实际低帧率的**代码逻辑问题 = 默认档位选择**：非白名单设备出厂默认 `vkd3d_limited_500k × dxvk_legacy(1.10.3)`，用户必须手动切 2.6.2 才有高帧率（2.6.2 在 SLG-W10 实测高，白名单的"未审计驱动"顾虑至少该机型实证不成立）。

**修复候选择（终局）：**

| # | 修复 | 理由 |
|---|---|---|
| 1 | **默认 dxvkBackend 改 `dxvk_modern_2_6`**（白名单逻辑去掉或反转：2.6 是默认，1.10.3 是显式回退） | 唯一根治"出厂即慢"；SLG-W10 实测 2.6.2 稳定高帧率 |
| 2 | UI 警戒：vkd3d × 1.10.3 组合提示"1.10.3 与当前帧管线不兼容，帧率损失显著" | 文档化 1.10.3 回退档语义 |
| 3 | legacy stable overlay 跳过机制键 + VN_PERF 短版（本次已验证无害，保留或回退均可） | 清理 env 噪音，非修复 |

---

## 8. 机理收敛：呈现后端默认转移（2026-08-30 静态复核补充）

### 8.1 结构证据：presentBackend 的默认值在两版本间被换掉

| | 1.0.12 (68f90d1) | master (d6c9575) |
|---|---|---|
| 默认值 | **硬编码 `"virgl_compositor"`**（`wine_exe.h:18`）且解析再兜底（`wine_exe.cpp:506` `GetString(...,"presentBackend","virgl_compositor")`） | `GetString("presentBackend")` 为空 → `DerivePresentBackend`（`wine_exe.cpp:330-335`）：`dxvk_*`/`vkd3d_limited_500k` → **`venus_broker_present`**；ArkTS 端同步不再传（`Index.ets:306` 注释明示） |
| 对 DXVK/VKD3D 用户生效路径 | virgl_compositor（GL 喂帧/SHM，ZC 不激活） | **venus_broker_present（ZC 直连必须）** |

`wine_exe.cpp:198-199`：`SetVulkanPresentMode(presentBackend == venus_broker_present || venus_direct_present)` —— master 上所有 dxvk/vkd3d 直启会话的 broker 都以 **Vulkan 呈现模式**启动；1.0.12 默认会话永远以非 Vulkan 模式启动。

### 8.2 机理：ZC（零拷贝）契约是 2.6 的资源；1.10.3 被迫挂在不匹配的契约上

代码链（master）：

1. `EglRenderer::TryAttachZeroCopySurface`（`egl_renderer.cpp:123-275`）：只挂与 `IsVulkanPresentMode()` **标志一致**的 surface —— `:188 surface.vulkan != wantVulkanSurface → continue`（vulkan 源只配 venus 模式）；`:173` 标志翻转即 `ReleaseZeroCopyBinding`。
2. surface 的 `vulkan` 标志来自 **virgl IPC 协议层**（`graphics_broker.cpp:556` 起，`QueryZeroCopySurfaces` ← `virgl_ipc::SurfaceQueryReply` 的 `kSurfaceVulkan` 标志）——即 guest 图形栈（DXVK 私有呈现协议）的性质；**2.6.2 才有该源**。
3. ZC 帧源 = `OH_NativeImage` + guest 侧 per-frame 通知（`OnZeroCopyFrameAvailable`）。若帧源不活：`UpdateZeroCopyFrame` 连续 8 次失败 → `BeginFallback`（撤 ready，guest 切 SHM）→ 新 SHM 帧到 `ConfirmFallback` → 下轮渲染循环再试 ZC → **每帧在 ZC 判定↔SHM 回退间抖动**（`zc_bridge.cpp:58-106` 状态机 + `egl_renderer.cpp:303-324`）。
4. 1.0.12 默认 `virgl_compositor`：`wantVulkanSurface=false`，vulkan 面被 :188 一律拒绝 → 永不走 ZC 契约 → 纯 SHM/GL 喂帧，1.10.3 的经典上传/隐式同步模型顺水推舟 → **高帧率（D 样本）**。

**一句话解释"同样 1.10.3，1.0.12 高、master 低"**：master 把 DXVK/VKD3D 的默认呈现后端从「GL 喂帧」换成了「venus 零拷贝直连」——后者是 DXVK 2.6 专属契约；1.10.3 在其中没有可用的帧源，每帧陷在 ZC↔SHM 同步抖动里，表现为提交率被钉死。这与 §7 四轮实测全部相容（E/F 同低因同为 venus broker 模式；2.6.2 高因有 ZC 帧源）。

### 8.3 判别实验（MEASURE-A，已部署 1.8 pad 2026-08-30）

在 master 上把 `dxvk_legacy` 强制回退 `virgl_compositor`（`wine_exe.cpp` 临时补丁），模拟 1.0.12 默认路径；其余不变。测「纯 DXVK 路由 × 1.10.3」：

- 帧率高 → 定案：**主因 = 呈现后端默认转移**，修复即「1.10.3 档位自动配 virgl_compositor」（或默认档整体改 2.6.2）；
- 帧率仍低 → 主因在合成器重构的 SHM 路径本身，需逐帧 profiling 再定位。


### 8.4 自动化验证尝试（2026-08-30，记录于此避免复走）

欲在无人工点击下自主验证 MEASURE-A，逐项探明设备约束：

| 通道 | 结果 |
|---|---|
| `hdc file send/cp` → app 沙箱（`.wine/...`/`drive_c`） | **拒**（SELinux/dynamic fs；shell uid=2000，app uid=20020217 专属） |
| media 区 `/storage/Users/currentUser/...` | shell 侧不存在（按 app 视图挂载） |
| `uinput -K` 键盘注入 → app 键盘链 | 键码 125(Win)/28(Enter) 注入成功但 **guest 无 InjectKey 日志**（OHOS 键盘事件未派发给无 IME 焦点的 XComponent） |
| `uinput -T` 触摸链 → CLICK-PIPE → InjectButton | **可用 ✓**（`click coordinate` 日志验证） |
| 返回手势回 Index（`-T -g` / `-P -s`） | 无效（桌面 XComponent 全屏 top-most 吞手势） |
| `aa stop` 停 WineWindowAbility 子窗 | 命令不存在（仅 force-stop，会把引擎一并杀掉） |

**结论**：桌面全屏覆盖 + 沙箱只读 + 无键盘 → 无 UI 自动化启动 smoke 的合法通道。MEASURE-A 的一次人工测成为唯一终审（预期操作：设置页「纯 DXVK 路由 × DXVK 1.10.3」+ 启动任一游戏 → 判帧率）。设备端采集器（proc_snapshot/`MW-TAKE`/`VIRGL-ZC` 日志）随 app 常驻，终审时无需额外准备。

### 8.5 自主测量结果（MEASURE-B，2026-08-30 凌晨 1.8 pad，无人值守）

改 Index.ets 加自动套件（app 启动 → 引擎 ready → 三组合循环启动 `C:\smoke\x64\winehua_d3d11_smoke.exe`（每档 150s 观察窗）；档位以 runWineProgram 直启参数逐档指定，**presentBackend 走 ArkTS 显式传参**（native :358-360 非空不派生））：

| 档位 | 启动 | 设备实测（hilog） |
|---|---|---|
| A 2.6.2×venus | 02:47:27 pid36685，env `DXVK_VERSION=2.6.2` | 无窗口（未见 640x480 commit）→ 固定帧未完成 |
| B 1.10.3×venus | 02:50:02 pid47824，env `DXVK_VERSION=1.10.3` | **ZC 完整生命周期**（02:50:23 attach key=205402515963927 → GPU_ACTIVE → `frame=1 signals=1 failures=0` → **02:50:33.055 ready revoked + detach**）：**attach 后只来了 1 帧信号，9.3s 后被撤销；此后无 SHM 帧** |
| C 1.10.3×virgl | 02:52:37 pid58319 | 无 ZC（virgl 路径不激活）；`MW-COMMIT toplevel#6 640x480 stored=1228800` **仅 1 次**后静止 |

**结论（修订 §8.1-8.2 的判定细则）：**

1. **B 档实锤"1.10.3 × ZC 帧源断供"**：ZC 层对 1.10.3 能 attach（帧源标记过关），但**帧信号只到 1 个就断**（后续 `ONFrameAvailable` 不再触发）→ 无直接消费 → ZC 通道空等。这确认 8.2 的机制假说（1.10.3 在 ZC 契约下无持续帧源）。
2. **C 档揭示更深一层**：master 上 1.10.3 的 SHM 回退路径（virgl_compositor）在 smoke 固定窗前**也不持续合成**（1 帧 commit 后静止，0.24 fps）。即 master 合成器的 SHM 路径**只为"变化驱动+全屏主窗"服务**，1.10.3 小窗帧序列得不到主动消费。**1.0.12（默认全 SHM 常流）对 1.10.3 则是顺滑的** —— 这就是"同一 1.10.3：1.0.12 高 / master 低"的第三重证据。
3. **smoke 场景不等同用户游戏场景**（2.6.2 在 smoke 下也未完成 30 帧，而游戏全屏下 2.6.2 实测高帧率），判定主场景差异 = 合成器消费策略（全屏主窗 vs 小窗/lazy），因此以上只作为**机制定案**，**用户端帧率终判仍需游戏场景**（人测 MEASURE-A 包或下一轮）。

**最终原因表述（三句话）**：
- 直接原因：master 把 DXVK/VKD3D 默认呈现后端从 `virgl_compositor`（GL 喂帧）换成 `venus_broker_present`（ZC 直连）——1.10.3 没有 2.6 的 ZC 帧源契约（设备实证：attach 后帧信号只有 1 个就断）。
- 次因：master 合成重构后 SHM 路径惰性化（变化驱动 + 仅主窗积极消费），1.10.3 的 SHM 回退在 master 上得不到 1.0.12 式的常流喂帧。
- 可操作结论：**1.10.3 在 master 上没有任何"恢复 1.0.12 高帧率"的配置面**；修复= 默认档 2.6.2（候选 1）+（可选）1.10.3 档位自动配 virgl_compositor（为回退档兜底，但如上 C 档实验显示其收益未在 smoke 场景显现，需游戏场景二次验证）。

### 8.6 白屏插装定论与回滚（2026-08-30 下午，1.1x-1.2x 现场）

「1.10.3 高帧率组合 = virgl_compositor 呈现」在设备实测(switch cube)被**证伪且回归**：

- 症状：窗口标题 `vkd3d_limited_500k - 0.0 FPS - frame 0`（**guest 只 commit 1 帧（MW-COMMIT stored=2582624）后不再推进**），画面白屏。
- 插装（DBG-GATE）：`[DBG-GATE] tl=1 noFrame reason=3 hasFrame=1 dirty=0` —— root 有帧数据但门控拦截；链路核查（MarkDesktopRootDirtyLocked 定义/转发/引用同源/CheckDesktopRootOnCommit 调用点）全部完好 —— **门控只是后验表现，真正断点在 guest 侧**：1.10.3 的 DXGI present 在 virgl 模式（host 拒收 vulkan 面）0 帧；在 venus 模式（MEASURE-B B 档）attach 后仅 1 帧信号即断 —— **master 的 guest present 完成信号协议只对 2.6 帧源持续供给**。

**结论（修订 8.5）**：master 上 1.10.3 **无任何配置组合可达高帧率**（低帧率=venus 单帧断供；virgl=0 帧白屏）。已**回滚** DerivePresentBackend 至 d3dBackend 派生（legacy → venus_broker_present，恢复"有画面、低帧率"基准），并移除全部插装。**达成 1.10.3 高帧率的正确杠杆 = guest present 信号协议对着 1.0.12 复原（vtest/venus present 服务端参数差异专项）**，挂后续任务。
