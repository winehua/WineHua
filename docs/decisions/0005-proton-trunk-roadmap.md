# 决策记录 0005：把 Proton 路线收敛为主干

> 决策日期：2026-09-25
> 决策结论：**以 Proton 路线（Valve Wine 树 + FEX + ARM64EC）为 OHOS arm64 的目标主干**，按 P0–P4 五阶段推进，三个卡点各设 go/no-go；卡点 B（Steam 是否可用）失败时改产品目标，而不是硬推技术方案。
> 最后核实：2026-09-25
> 相关文档：[0003-three-schemes.md](0003-three-schemes.md)（三方案为什么共存）、[../architecture/platform-ohos.md](../architecture/platform-ohos.md)（平台层）
> 证据出处：本文的数字和结论来自 `feature/proton-wine-ohos` 分支（工作树 `/data/share/wine-arm64`）的一次完整调研。原始记录在该分支的 `docs/proton-wine-core-migration/`（迁移台账与风险图）、`docs/steam-win64/`（真机验证与交接）、`docs/PROTON_OHOS_RUNTIME_NOTES.md`（运行须知与未结清单）。

## 一、这个决策要解决什么

现在是两条平行的线：

| | 现在的 `master` | Proton 线（`feature/proton-wine-ohos`） |
|---|---|---|
| 跑什么 | 方案① x86_64 原生、方案② box64 + WineHQ fork | 方案③ arm64 原生 Wine + FEX |
| Wine 树 | `thirdparty/wine`（winehua fork，Wine 11.10） | `thirdparty/wine-valve`（内容为 Valve proton_11.0） |
| 状态 | 可运行、已产品化 | 能起引擎和桌面；Steam 能装能登能出界面；卡在 CEF 与图形链 |

产品要的是「Proton 的能力，跑在 OHOS arm64 上」。`master` 那条线给不了——它是 WineHQ fork + box64，跑不了 Steam win64，也没有 ARM64EC 这条 64 位路径。所以方向只有一个：**收敛到 Proton 线**。本文记录的就是怎么收敛。

## 二、先把「和 Proton 对齐」的边界说清楚

Proton 是五件套：Valve Wine + FEX + DXVK + vkd3d-proton + Steam 运行时集成。其中能对齐的和不能对齐的必须分开：

| 层 | Proton（Linux 宿主） | 我们在 OHOS 上 | 能否对齐 |
|---|---|---|---|
| Wine 核心 | Valve wine（proton_11.x） | `thirdparty/wine-valve` | 已对齐（血缘待修，见 P0.5） |
| 指令转译 | FEX（ARM 场合） | FEX | 已接，但落后上游 4 个月（见 P0.1） |
| D3D → Vulkan | DXVK / vkd3d-proton | 自 fork 的 dxvk + 自搓 arm64x | 应回归官方 pin（见 P1） |
| Vulkan 呈现 | 原生 Vulkan 驱动 + wsi | guest Mesa Venus → vtsocket → virglrenderer → 宿主 Vulkan | **不能对齐** |
| 输入 / 窗口 | gamescope 或桌面环境 | 自研合成器 + winewayland 私有扩展 | **不能对齐** |

后两行是 OHOS 沙箱决定的：没有直接 GPU 访问、没有 DRM/KMS、没有现成的 Wayland 显示服务器。**这一节的用处是排错时先定位分层**——落在后两层的故障，Proton 那边没有对应实现可参考，只能自己解决。当前卡住的图形链问题大概率在这一层。

另外一条已经明确的事实：**Valve 的 ARM64 路径和我们不是同一条**。Valve 走的是 SteamOS ARM64 + Linux 原生 Steam 客户端 + 游戏经 Proton/FEX；我们走的是 Windows 版 Steam 客户端 + Wine ARM64EC。CEF 官方对后者没有目标平台（只有原生 ARM64），并且明确不建议在模拟下跑。**这条路的上游红利只覆盖游戏兼容性，不覆盖 Steam 客户端本身。**

## 三、路线全景

四条泳道并行，五个阶段串行设卡：

| 阶段 | A·技术攻坚 | B·工程基座 | C·资产合流 | D·产品定义 |
|---|---|---|---|---|
| **P0 立基**（1–2 周） | FEX 统一到 2609+、剥诊断补丁 | CI 覆盖、从零 make 演练、wine-valve 血缘、双 wine 树收口 | 冲突面试算（已完成） | **验收标准 + 目标游戏清单** |
| **P1 攻坚**（4–8 周，时长取决于 CEF） | CEF 稳定性、图形链、音频、网络 IPC | 构建目录身份校验、诊断件清理 | 主线 16 冲突合并 | 阶段性可玩清单 |
| **P2 合流**（2–4 周） | 三方案①②在新树上的裁决 | 回归门禁（smoke 全套） | **wine 侧 6–8 项移植 + 功能回归** | 兼容性对照 Proton |
| **P3 切换**（2–3 周） | 设备矩阵回归 | 发布链切换、双树退役 | master 退役 / 转向 | 对外口径 |
| **P4 可持续**（长期） | 跟进 Proton 发版 | 补丁重放流程化 | — | 兼容性数据库 |

## 四、P0 立基（1–2 周）

这一阶段必须全做完才能进 P1——不做，P1 的任何观测都不可信。

| # | 工作项 | 验收 |
|---|---|---|
| P0.1 | **FEX 统一基线**：以 FEX 2609+ 为准，重打 7 个行为补丁、剥离 14 个诊断补丁，重建两个产物 | 设备 / 容器 / 文档三处 `sha256` 一致 |
| P0.2 | **FEX 升级后重测**：CEF 崩溃率、PAL4 帧率、32 位 steamapp | 与 2605 基线有可比数据（卡点 A 的输入） |
| P0.3 | **从零 make 演练**：干净 clone → `make NATIVE_ARCH=arm64-v8a` → 出包 | 全绿，无人工干预 |
| P0.4 | **CI 覆盖**：proton 分支加进 `build-arm64-native.yml` 触发列表；先定并线后 `master` 走方案②还是③，再改 `build.yml` 的断言 | CI 跑绿一次全量 |
| P0.5 | **wine-valve 血缘修复**：`git merge -s ours valve/proton_11.0`，把 Valve 的提交接进历史 | `merge-base` 有解，之后能增量合并上游 |
| P0.6 | **双 wine 树收口**：明确 `thirdparty/wine` 的去留 | `.gitmodules` 与实际编译树一致 |
| P0.7 | **构建目录身份校验**：构建目录和 stamp 落 `srcdir` 记录，不匹配即重配 | 主/支线来回切换不会静默复用旧树产物 |
| P0.8 | **产品定义**（泳道 D）：验收标准 + 目标游戏清单 | 有可判定的"对齐"定义 |
| P0.9 | 杂项：`make test` 路径失效、`patches/wine/0003-0005` 死补丁、脚本里的作者本机绝对路径 | 清理完毕 |

FEX 为什么会落后：我们的基线是 `FEX-2605-2`（2026-05-10），上游已到 2609（2026-09-08）。这中间上游修了多个直接产生 SIGSEGV 的 ARM64EC 机制（2605 的挂起门铃竞态、2608 的 AVX 信号状态保存/恢复、2609 的 SMC 检测加速），我们一个都没吃到。同时设备 / 容器 / 文档里还流转着三份不同 SHA 的 FEX 二进制——P0.1 一次解决"内部不一致"和"落后上游"两个问题。

**卡点 A（P0 结束）**：FEX 升级后 CEF 崩溃率有没有变化？
- **有变化** → 症结在 FEX，P1 沿这条线深挖；
- **没变化** → 排除 FEX 一大块，转向上游对照（FEX 官方推荐 `bylaws/wine` 的 `upstream-arm64ec` 分支，那是为 FEX 场景维护的树）。

## 五、P1 攻坚（4–8 周）

时长完全取决于 CEF 那一条线，其余项相对可估。

**A 轨道 · CEF 稳定性（关键路径，决定全局）**
- 分层归因：现在有三类首发证据（V8 反优化栈帧、IPC 超时、Metrics 退出清理 TLS），但还没做"问题在 ntdll / FEX 翻译 / V8 JIT 的 ARM64EC 后端 / CEF 自身假设"的归因；
- 上游对照：Wine 11.16（2026-08）改进了 ARM64EC 异常处理（`CONTEXT_ARM64_X18`、context flags 转换保留异常标志、SIMD 异常码），要核对 Proton 11 分支是否已吸收；
- 验收：**Steam 四项口径**——原 `vulkan-1.dll` 保持原位、自检不反复下载、CEF GPU 不进崩溃循环、**界面实际操作有响应**；外加三次冷启动通过。"截图出现界面"不算通过。

**A 轨道 · 图形链（独立的第二命门）**
- 分层定位 present 失败与"内容区黑"落在 DXVK / Venus / virglrenderer / 合成器 哪一层；
- DXVK 决策：回归官方 pin，还是继续 fork + 手搓 arm64x；
- vkd3d：d3d12 固定 x64 单图（放弃 arm64x）的后果评估。

**A 轨道 · 其余（可与 CEF 并行）**
- 音频 M3（子进程 `exit=5`，故障 PC 尚未归位到模块）；
- 网络 / TLS / 多进程 IPC 的 Gate 用例**从零补**（现在不存在，M3 因此无法判定）；
- PAL2 黑底白条、`addr=…0xff0` 的 `SEGV_ACCERR` 良性判定、SMC 三级链、Steam UI 线程卡死、合成器坐标空间收口。

**B / C 轨道**
- 主线 16 个冲突文件合并（含 smoke v1 / v2 的取舍）；
- 诊断件清理：`WINEHUA_CEF_NO_CRASH_HANDLER`、`WINEHUA_WX_RECOVER`、jitless / no-opt、raw rt_sigaction、诊断版 FEX DLL——一律不进产品包。

**卡点 B（P1 结束）**：Steam 是否达到"能装、能登、能操作、三次冷启动通过"？
- **是** → 进 P2；
- **否** → 触发第九节的 B 计划。这是整条路线里唯一可能推翻产品目标本身的决策点。

## 六、P2 合流（2–4 周）

把 `master` 的资产合过来，两条线并成一条。工作量已经实测算过：

| # | 工作项 | 已算出的量 |
|---|---|---|
| P2.1 | 主仓库 16 个冲突文件解决 | 2–5 人天（真正要判断的约 6 个，其余机械） |
| P2.2 | **wine 侧 6–8 项 master 修复移植** | 5–10 人天 |
| P2.3 | 功能回归：modal、compositor 十项、PC 拖拽 resize、Pad Fusion、dxvk d3d10 链、500k 环境变量 | 逐项真机 |
| P2.4 | smoke v2 全套接管（替代 v1） | 套件全绿 |
| P2.5 | 三方案①②在新树上的裁决：Proton 树能否编 x86_64 宿主？box64 路线保留还是退役？ | 明确结论 |

P2.2 的 6–8 项里，有两项是**加壳程序兼容**的硬修复，不能省：

- `ohos_anon_replace_page`（+110 行）：处理"加壳程序把整个镜像改成 RWX、连 `.data`/`.rdata` 这类非可执行节也要 PROT_EXEC"；wine-valve 上只有 `ohos_map_exec_section`（映射时用匿名），缺"事后把已存在的文件页换成匿名页"这一半。
- PE 头页改 `pread`（+12 行）：PE 头不属于任何节，走不到匿名映射路径；若在此处建文件映射，加壳 stub 把模块改成 RWX 时会在 PE 头页上失败。

其余几项：modal 支持（`modal.c` + 私有协议上报）、winebus 手柄总线（`bus_ohos.c`）、窗口最外 1px 圈、wayland 尺寸可调整性判据、rpcrt4 的 ARM64EC shadow space（这条在 parity 分支上真机验证过 RPC 闭环）。

**移植时不能照抄 diff**：两棵树基线不同（11.0 vs 11.10），`win32u/defwnd.c`、`ntdll/unix/virtual.c` 是上游文件，要按语义重放。

**卡点 C**：全量回归（smoke 全套 + 设备矩阵）通过。

## 七、P3 切换（2–3 周）与 P4 可持续（长期）

P3：全量回归与设备矩阵（平板 / 手机 / 开鸿 PC / 模拟器）→ `build.yml`、`build-arm64-native.yml`、发布链切到新主干 → `thirdparty/wine` 退役 → `master` 转向（保留一个版本周期作保底）。

P4：建立上游跟进机制（Valve Proton 约两月一发版）；把"哪些游戏能跑"沉淀成可累积的兼容性资产。**这里要还的债是适配层厚度**——现在 WineHua 在 Wine 树上的自有改动是 61 个提交 / 101 个文件，其中 61 个是改上游文件的，每次跟进都要重放。长期方向是让 OHOS 特有的东西尽量下沉到合成器 / 宿主侧，Wine 树里的改动越薄，跟上游越省力。

## 八、全部已知问题落在哪一阶段

这张表保证没有遗漏：

| 问题 | 落在 |
|---|---|
| CEF `signal 11`、Steam 四项口径、三次冷启动 | P1-A（卡点 B） |
| Steam 主窗口内容区黑、从 Steam 启动游戏未验证 | P1-A 图形链 |
| 32 位 steamapp 崩溃、PAL4 自动 LAA 未打包、默认 BIGBLOCK=3 未验 | P1-A |
| box64 回退档卡死 | P1-A（或随 P2.5 一并裁决） |
| DXVK present 失败、vkd3d 白屏 | P1-A 图形链 |
| 音频 M3、网络 TLS、IPC Gate 用例缺失 | P1-A |
| PAL2 黑底白条、坐标空间收口、`0xff0` 良性判定、SMC 三级链、UI 线程卡死、ProcessorMetrics TLS 崩溃、T1 只读页、透明窗口不支持、全零 SHM 遮挡 | P1-A |
| FEX 三份 SHA、补丁混杂、落后上游 4 个月 | **P0.1** |
| 从零 make 未演练、CI 零覆盖、`build.yml` 与新语义冲突 | **P0.3 / P0.4** |
| wine-valve 血缘断裂、双 wine 树、跨树构建目录复用 | **P0.5 / P0.6 / P0.7** |
| 主线 16 冲突、wine 侧 6–8 项、modal / compositor 等功能回流 | **P2.1 / P2.2 / P2.3** |
| smoke v1 与 v2 的取舍 | P2.4（P1 内先做取舍决策） |
| 诊断件不得产品化 | P0.1（FEX 侧）+ P1-B（清理） |
| 三方案①②前途、DXVK fork 去留 | P2.5 / P1-A |
| 单人开发、无 PR、开发停在另一台机器 | 贯穿全程——这是唯一的非技术障碍 |

## 九、卡点与 B 计划

三个卡点：

| 卡点 | 时机 | 判据 | 不通过怎么办 |
|---|---|---|---|
| **A** | P0 结束 | FEX 升级后 CEF 崩溃率是否变化 | 变化则深挖 FEX；不变则转 `bylaws/wine` 对照 |
| **B** | P1 结束 | Steam 四项口径 + 三次冷启动 | 触发 B 计划（见下） |
| **C** | P2 结束 | 全量回归 + 设备矩阵 | 回到 P1/P2 定位，不放行切换 |

**B 计划**（卡点 B 失败时改目标，不是硬推）：

- **B1 保游戏、不保 Steam 客户端**：保留 Proton 的游戏兼容性（DXVK / vkd3d / ARM64EC），用自研的游戏管理器替代 Steam 界面，游戏资源走 SteamCMD 下载。
- **B2 接受受限 Steam**：Steam 能装能登、游戏经外部入口启动，客户端界面不可用。
- **B3 退回 box64 路线再补 Steam**：成本更高、性能更差，且 box64 回退档本身有未结案的卡死——**不建议**。

## 十、风险登记（按可能推翻路线的程度排序）

1. **CEF 是无人区**（最高）。公开信息里没有人做成过"Wine ARM64EC + Windows 版 Steam 客户端 + 非 Linux 宿主"这个组合——没有成功案例，也没有同路径的失败报告可对照。上游对 steamwebhelper 崩溃只有**缓解**（FEX 2603 的原话是"关掉 FEX 日志能降低发生概率"），到 2609 仍未结案。另有旁证指向 **V8 JIT 的 ARM64EC 后端本身不完整**（原生 ARM64EC 编译的 `d8.exe` 一跑就崩、加 `--jitless` 正常）——这一块不在我们能控制的范围里。
2. **Valve 不会替我们走这条路**。Valve 的 ARM64 是 Linux 原生 Steam 客户端 + Proton，我们这条 Windows-Steam-on-Wine 的路径拿不到那部分上游红利。
3. **图形链的结构性差异**：Venus → virglrenderer 的转发层是我们独有的，Proton 的经验不能直接搬。
4. **上游节奏**：Proton 约两月一发版、FEX 约每月一版；我们的补丁层越厚，跟进越吃力。
5. **单人依赖**：这条线目前没有 CI、没有 PR、开发在另一台机器上。P0.4 / P0.5 做完之前，它不具备"主干"应有的工程形态。

## 十一、待定的输入（P0.8，现在就该定）

1. **"与 Proton 对齐"的可判定标准**：ProtonDB 金 / 白金？Steam 某类目 Top N？还是 D3D 特性矩阵达标？
2. **目标游戏清单**：决定 P1 图形链的优先级（D3D11 先行还是 D3D12、32 位还是 64 位）。
3. **资源投入**：P1 的 CEF 与图形链是两条独立战线，一人串行做会显著拉长周期。

## 十二、一句话总结

方向已经确定，剩下的是把一条**被证明走得通、但没走完**的实验线，变成**有基线、有门禁、有验收**的主干。三个卡点里，卡点 B（Steam 是否可用）是唯一可能改变产品目标的；其余都是工程量，可估、可控。
