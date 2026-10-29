# 模块清点与可解释性台账

适用场景：制定重建式升级路线、评估某块定制的可信度、升级前排查「迁移时会丢什么」时看这篇。
最后核实：2026-09-25（11 组并行清点，逐笔对照 diff、当前代码与定制文档）。

## 分类口径

每个定制点/机制归入一类：

- **A 机制正确**：有机制级解释（平台为何如此、为何必须这样改），方案站得住
- **B 治标待治本**：能解释当时为什么这么做，但属止血/特判/绕过，有已知或疑似的更好做法
- **C 已失效**：针对的问题已不存在（被取代/场景消失/死代码）
- **D 不可解释**：从代码、注释、提交信息、文档都无法还原「为什么必须这样」

## 总览

| 模块 | 规模 | 可解释率 | 结论 |
|---|---|---|---|
| wine 适配 | 83 笔非 merge（相对 origin/dev） | ≈95%，D 类 2 | 已收敛到 ohos_*.c 平台层 + winewayland 定制线，重构系列验证平价可整体搬运 |
| 宿主 compositor | 9.5k 行 / 54 文件 | 19 机制 16A | 注释即文档密度高，不必重写，按 0001 决策定点补缺口 |
| 宿主 input | 6.8k 行 / 23 文件 | 14 机制 12A | 机制解释质量高；keymap blob 无再生链是硬缺口 |
| 宿主 graphics | 6.6k 行 / 17 文件 | 18 机制 16A | 全库解释质量最高；少量死代码与未开关化的诊断 |
| 宿主其余（wine/proc/bridge/audio） | ≈9k 行 | 多数 A | 进程模型/音频设计扎实；WINEDEBUG 死通道、测试特判进产品 |
| ArkTS 层 | 8.7k 行 / 26 文件 | 服务层多数 A | 疑点密度最高：死守卫、四份手势复制、机型白名单 |
| virglrenderer 定制 | 38 笔（基线 8cb58e47） | 38/38 | 每笔有机制理由，债在诊断夹带功能与手改生成文件 |
| dxvk legacy 定制 | 33 笔（基线 v1.10.3） | 33/33 | Venus/Maleoon 能力兜底成体系；分支状态有地雷 |
| dxvk-modern 定制 | 3 笔（基线 v2.6.2） | 3/3 | 比 legacy 演进（按真实能力请求），样板模块 |
| box64 定制 | 15 笔（基线 8f445d9a0） | 14/15 | musl 兼容链完整；rint wrap 与 mallochook 是欠账 |
| 构建链 | Makefile + scripts/ | 机制层 A | 增量/stamp/断言设计好；从零重建仍有 8 处会卡 |

**总体结论**：存量定制可解释率约九成，大爆炸式重写缺乏依据——绝大多数适配对应真实平台机制，重写只会重付学费。支持渐进式模块升级路线，且存在明确的「直接搬运层」（见文末）。

## 全局风险清单（跨模块模式）

1. **提交分类不可信**（重建迁移时最易误删）：
   `f86d066b9bc`（wine，标题 smoke 实夹带 vkd3d `HLSL_TYPE_HALF` shader 编译变更）、
   `5058927a`（dxvk legacy，标 diag 实含 present sRGB 修复）、
   `39344384`（virgl，标 diagnostic 实含 shadow generation 功能锁）、
   `152b762c`（virgl，手改 generated venus 协议头，合并必丢）。
   迁移这四笔前必须把功能部分摘出独立成笔并补机制说明。
2. **跨仓字面契约无单一来源**：`vkd3d_limited_500k` 至少 5 处（wine `loader.c`、`wine_exe.cpp`×3、`WineEnvService.ets`、`wine_env.cpp`、`EntryAbility.ets`）；Z: 盘符 ntdll 走 `$HOME` 而 `mountmgr.sys` 硬编码 `/storage/Users/currentUser`；音频协议版本 `wine_child.cpp:499` 硬编码 `"1"` 与宏双源。
3. **构建可复现缺口**：`keymap_xkb.h` 2854 行 hex blob 无生成脚本；SDK 路径硬编码 `/apps/harmony`；`wayland-scanner` 需装系统目录；`ld.lld` 依赖手工软链；代理配置不在脚本；FFmpeg 失败静默跳过（包无解码器但构建绿）；`assemble.sh` 库清单硬编码到 patch 版本号、缺库只 warn；发布四步全手工。glib/gstreamer 的源码适配由构建脚本负责（`patches/glib-format-security.patch` + 幂等 sed），submodule 里 staged 的同内容改动是残留，应丢弃。
4. **submodule 分支地雷**：`.gitmodules` 声明 dxvk 分支 `dxvk-legacy-1.10.3`，本地无此分支，工作树 detached 在定制头 `5058927a`，而本地 `master` 指向上游 3.0.2——在此仓库 `git checkout master` 会丢全部 33 笔定制。
5. **测试进产品路径**：`wine_child.cpp:100` 按进程名特判 `mj_x86.exe`/`winehua_audio_test.exe` 换 WINEDEBUG；ArkTS `[[SMOKE]]` 6 处钩子在产品链上；`WineWindowManager.ets:647` move_end 后 2000/3000ms 诊断 setTimeout（白屏取证残留）。
6. **设备/机型特判当配置**：virgl 按 vendorID 0x19e5 自动开 shadow GPU upload；x86_64 编译特判最小 EGL 属性；`EntryAbility.ets:20` 机型白名单（`VYG-AL00 && 26.0.0.32`）钉 dxvk_modern，系统升级即静默回退 legacy；`WineEnvService.ets:328` perfProfile 写死实验档。

## D 类清单（不可解释项，升级时须逐个实测验收，不许默认平移）

| 项 | 位置 | 现状 |
|---|---|---|
| schannel CHACHA20 禁用 | wine `schannel_gnutls.c:437`（`57bad821`） | 提交仅标题「Box64 下强制 AES-GCM」，无机制正文、无文档 |
| nulldrv_CreateWindow 特判 | wine `driver.c:1034`（`7718da97c1c` 内） | OHOS 下把无驱建窗从 nodrv 换 nulldrv，无解释 |
| rint wrap | box64 `23750c922` | 上游注释态被无正文启用，同类 nearbyint 未开 |
| venus 源 flipY | `graphics/egl_renderer.cpp:37` | ComposeZeroCopySamplingTransform 对 Vulkan 源翻 Y，全文无为何 |
| WINEHUA_WINEDEBUG 通道 | `wine_child.cpp:458` 读取早于 `:479` __env 应用 | master 永不可达；`:328` 注释仍指引走此通道 |
| keymap_xkb.h 再生链 | `input/keymap_xkb.h` | 无生成脚本，改布局无法从零重建（流程性 D） |
| PersistentStorage 冷启动 | `WineEnvService.ets:174` | 「冷启动不恢复，原因未查明，绕开」，两套持久化并存 |
| rawDelta ±512 钳制 | `input_manager.cpp:383` | 「125Hz 合法事件远小于此」无测量依据 |

## 各模块要点

### wine（83 笔）

平台机制已收敛到 `dlls/ntdll/unix/ohos_virtual.c / ohos_broker.c / ohos_file.c` 三文件（noexec 三连、broker 进程链、盘符映射），上游文件只剩单行注入点，可整体平移。窗口/输入线（winewayland、受管判据、可调整性判据、标题栏正方形、相对指针、手柄 opt-in）全部机制级实锤。重构系列（M1-M5、`__OHOS__` 守卫、`__env__`）逐笔核查行为平价，可作直接搬运层。

- B 类：模拟分辨率 per-process 短路（「实测 vd=0」未查根因）、vkd3d 私有 swapchain 无条件加 TRANSFER_DST（不分档位全局生效）、loader overlay 的 `vkd3d_limited_500k` 字面特判、相对模式 enter 静默校准（`WINEWAYLAND_ENTER_SILENT` 宿主固定注入 `=1`，事实默认路径而非排查开关）。
- 可直接丢弃：`ae50645ad47`（WGL_FORCE_GLES，已被删）、`910056eecb7`+`8ae9f9b62b7` 的 env 白名单（被 `__env__` 取代）、`c96fc9d9931` 与 `b74b2bd3a81`（同一补丁重复提交）。
- C 类：Z: 与 HOME 解耦（场景已消失）。

### 宿主 compositor（9.5k 行）

架构不需要重写。三条 B 类链有同一治本入口：wine 侧补 `unset_minimized`、改调 `set_maximized`、只对前台窗口发 `set_fullscreen`，可一次性消掉 fsPriority 仲裁、最小化按尺寸猜（commit>200x50）、max_size 启发式三条特判链。地基级问题：`region_subtract` 计数递增而非递减（靠「Wine 只判空/非空」兜底）、`set_input_region` 不走 commit 双缓冲但参与 InlineClient 路由判据、frame callback 时间戳用墙钟 `time(nullptr)*1000`（非单调）。死代码 4 处（`CommittedSurface` 三个只写字段、`hasSizeLimits`、`SubsurfaceAsLayer()`、`ZOrderNeedsParentPosCheck`）；两处自述 P2 未修（popup 父亡竞态、place_above 不映射 PC 子窗 z 序）。

### 宿主 input（6.8k 行）

机制解释质量高（多数带实测依据与失效模式说明）。`controller/` 手柄子系统 1255 行（约占该组 1/5）在 `docs/architecture/input.md` 零覆盖。B 类：脉冲拉伸 STRETCH 补 100ms（有 PAL2 帧时长推算但无线程序论证）、warp/约束协议有意不校验 serial、`set_cursor` 空实现无代价论证。竞态候选：`text_input.cpp:266` 锁外读 state_。死代码：`SendDeleteSurrounding` 无调用方。

### 宿主 graphics（6.6k 行）

16/18 机制 A 类且多带实测日期。B 类：2500ms attach 阻塞持锁等待、virgl_child 20ms+`_exit(0)` 关停赌 IPC 落盘、ZC 降级 8 次/100ms 魔数。死代码：`virglServerProgramPath_`（只打日志不用于启动）、NCP-waitpid 恒死分支。诊断未开关化：PIX-SAMPLE 每 30 帧跑在渲染循环、perf forwarder 250ms 常驻线程。

### 宿主其余（wine/proc/bridge/audio，≈9k 行）

进程模型（broker 单通道、NCP 退出回调权威、wineserver 三态锚点、热重启保锚点）与音频（memfd ring 零拷贝、两级控制面、来电 RESUME 重启）设计扎实。问题：NCP 抽象只覆盖 spawn/create，kill/退出回调走手工分支（两套分流）；`SetHostShadowProfile` 档位映射与 env_profiles 两处重复；`env_profiles.cpp:130` 输入输出别名靠注释纪律保证；`phone_adapter` 手写双份线协议无版本号。

### ArkTS 层（8.7k 行）

10 个服务职责与状态机清晰，但疑点密度最高：

- `desktopLauncherVisible` 死守卫：写入方 `setDesktopLauncherVisible` 零调用、`VirtualDesktop.ets:48` 引用不存在的 `Index.onLauncherVisibleChange`、`DesktopLayer.ets:36` 守卫恒 return——root resize 通知永不发，机制名存实亡
- 手势状态机四份复制（DesktopLayer/DesktopWindow/WineWindow/WinePopup，≈1500 行）；`DesktopWindow.ets:323` rawDelta 漏乘设备系数（`DesktopLayer.ets:607` 已修并自注漏乘），同一鼠标两套手感
- decor 校准机制已失效（barH=0）但 EMA、surface 校准、1.61 魔数回退全套保留
- `desktopAbilityRunning` 只写不读；墓碑 Set 永不清理
- `docs/architecture/overview.md` 正档完全没有 ArkTS service 层描述

### virglrenderer（38 笔，基线 8cb58e47）

EGL 无 GBM 初始化、vtest 封装 dlopen 库、present 桥私有命令、shadow 内存桥（Maleoon 无 dma-buf 导出的唯一路径）、Z32 回填仿真、sRGB 判定全部实锤。B 类：busy-wait+切上下文 glFinish 兜底（治本在驱动）、设备名特判自动开 GPU upload、x86_64 特判 EGL 属性、功能锁夹在 diag 提交。15 笔纯诊断提交 env 门控完好，可整体剥离。`152b762c` 手改 generated 协议头是合并地雷。

### dxvk legacy（33 笔，基线 v1.10.3）

Venus 兜底主体（BC 解压、combined sampler、event query→Fence、staging flush、instance divisor CPU 展开、RGBA8_SNORM 转 RGBA16F、单采样 A2C）全部实锤且文档一致。B 类：`WINEHUA_DXVK_RELAXED_FEATURES` env 放行 22 项（2.6 已演进为按能力请求）、Heaven 专用 compare ALWAYS 特征匹配 hack（默认关）。14 笔诊断提交全 opt-in。分支状态见全局风险 4。

### dxvk-modern（3 笔，基线 v2.6.2）

兜底移植 + flush 合并 + swapchain factory 回退，全部 A 类且比 legacy 演进（自动门控 `isWineHuaVenus()`、按真实能力请求、守卫 2.6 加速路径）。模块升级的样板。

### box64（15 笔，基线 8f445d9a0）

musl 兼容链（弱符号桩、fts/obstack 移植、7 符号桥接救活 gnutls/gstreamer）、沙箱无 execve 的 `box64.so`+dlopen 方案、prctl 残留地址 SIGSEGV 修复、BOX32 低 4GB bump 堆，全部实锤。欠账：rint wrap（D）、mallochook 诊断版滞留生产 3 个月（根因未实锤，tcmalloc 永久丢失）、`threads.c:878/890` 与 `threads32.c:812/822` 裸调用已置 NULL 的 cleanup 指针。explorer metadata 挂死的根因修复不在本仓（主仓库 `94b09db` env 兜底）。

### 构建链

单入口阶段链、stamp+`find -newer` 增量、产物断言、wine-data.zip manifest 重打包、架构隔离、tarball 幂等下载——机制层全 A。缺口见全局风险 3。`docs/build/env.md` 仍推荐与 Makefile 平行的第二入口 `build.sh`（默认 IP 硬编码），双入口漂移。wine.inf 140 行字体映射以 sed 埋在打包脚本而非 wine 源。

## 重建路线含义

1. **直接搬运层**（机制实锤 + 已验证平价）：`ohos_virtual/ohos_broker/ohos_file` 三文件及注入点、compositor 架构本体、宿主 graphics 的 A 类机制、wine 重构系列、virgl 1-8/12/13 项、legacy DXVK 3-8 项、dxvk-modern 全部、box64 musl 兼容链。
2. **迁移前必须处理**：全局风险 1 的四笔夹带提交拆笔重述；`keymap_xkb.h` 生成命令固化进 `scripts/`；丢弃 glib/gstreamer staged 残留。
3. **升级时改机制不做特判平移**：D 类清单逐项实测；virgl/EntryAbility 机型特判改能力探测/配置项；wine 三处协议补齐（unset_minimized/set_maximized/前台 set_fullscreen）；`vkd3d_limited_500k` 等字面契约收单一来源。
4. **可丢弃清单**：wine 3 笔（见 wine 节）、15 笔纯诊断提交（重做诊断时按需重写）、ArkTS 死守卫/decor 残留/诊断 setTimeout、宿主死字段死分支、`build.sh` 第二入口。

## 文档修正清单（本次清点发现的出入）

| 文档 | 问题 |
|---|---|
| `docs/customization/wine.md` | `037984bdc80` 的解释（图标位裁剪）与真实机制（-32000 哨兵被覆盖）不符；`752ef4aefcb` 动机记偏（实为 dinput 差分污染，非视角突跳），且未记 `WINEWAYLAND_ENTER_SILENT` 固定注入；缺 `cf39fdf1df6`、`5da768ffdd4` 两笔 |
| `docs/architecture/platform-ohos.md` | Z: 映射口径与 `mountmgr.sys` 硬编码不一致 |
| `docs/architecture/opengl-virgl.md` | 宿主 EGL 库来源与 `WINEHUA_VIRGL_LOG_PATH` 读取方两处写错；venus presenter 链路与 resize 判定无落点 |
| `docs/architecture/compositor.md` | 「锁外绘制」红线只对 Desktop root 成立，`WindowFrameComposer` 全程持锁（有实测崩溃理由）未提 |
| `docs/architecture/input.md` | `controller/` 子系统零覆盖；`Input-DROP` 统计对象未说明；相对对象多实例计数解锁条件缺 |
| `docs/architecture/audio.md` | 来电 RESUME 恢复、capture 惰性启停未记 |
| `docs/architecture/process-model.md` | napi 名写错（实际 `nm_modname="entry"`）；WINEDEBUG 通道在 master 不可达未标 |
| `docs/architecture/overview.md` | 缺 ArkTS service 层一节 |
| `docs/debugging/observability.md` | 界面层标签缺 FusionWM/PopupWM/ModalWM/VDA/VD/LIFE/`[MW]PtrVis` |
| `docs/glossary.md` | arm64 方案 ARM64X 叙述与 master 构建树不符（产线在 feature/arm64） |
| `docs/customization/box64.md` | HEAD 分支位置过时（origin/master 已等于定制头） |
| `WineWindowManager.ets:700,1194` | 注释引用的 docs 路径已移 archive |
| 代码注释尝试史 | `opengl_readback.c:426`「曾潜伏 7 天」、`input_manager.cpp:230`「旧代码曾误将此值交换」 |
