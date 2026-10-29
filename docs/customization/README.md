# 开源模块定制总览

> 适用场景：想知道"为了在鸿蒙上跑起来，各开源模块都改了什么、为什么改"；评审 submodule 改动、排查跨模块问题时定位定制点。
> 最后核实：2026-09-25
> 相关文档：[architecture/platform-ohos.md](../architecture/platform-ohos.md)（平台限制的原理）、[architecture/contracts.md](../architecture/contracts.md)（跨仓库私有协议）、[architecture/graphics-matrix.md](../architecture/graphics-matrix.md)（档位与通道）

本目录每个文件讲一个模块的定制：每个定制点先说**解决了什么问题**，再说**怎么解决的**。逐文件、逐 hunk 的合并级明细在各篇"补丁清单"部分。

## 平台差异从哪来

Wine 假设自己跑在一个标准 Linux 发行版上。鸿蒙应用沙箱跟这个假设差得很远，定制基本都源于下面这些差异：

| 差异 | 具体表现 |
|---|---|
| 进程模型 | 沙箱内没有 `execve`，创建子进程要走宿主的 Process Broker；wineserver 这类辅助进程不能自己 fork |
| 内存权限 | 数据分区 noexec，文件映射加不上 `PROT_EXEC`；匿名 RWX 映射被内核拒绝；页权限在创建时确定，`mprotect` 返回 0 也可能没生效 |
| libc 是 musl | 一批 glibc 私有符号不存在（fts、obstack、`__ctype_*_loc`、`epoll_pwait2`、`RTLD_NEXT` 语义等） |
| 文件系统 | 沙箱没有 `symlink()`，Wine 的 `dosdevices/c:` 盘符布局立不起来；路径布局也与发行版不同 |
| 图形 | 没有 X/Wayland 系统合成器，画面要送到 `OHNativeWindow`；宿主 GPU 是 Maleoon，Vulkan 驱动（Venus 虚拟化通道）缺一批特性和扩展，还有若干驱动缺陷 |
| 音频 | 沙箱内不能直接调 OHOS 音频 API，要经 IPC 交给宿主进程混音 |
| TLS | 没有 openssl 系统库，Wine 的 schannel 后端需要自带整条 gnutls 依赖链 |

## 图形链全景

图形是定制最集中的地方。一条 Vulkan 帧（DXVK 游戏）经过的每一环都有定制：

```
D3D11 程序
  → DXVK (fork)            D3D11→Vulkan；补 Venus 缺的特性（BC 纹理、双源混合等）
  → wine win32u            私有 swapchain：present 走项目私有通道，不走宿主 WSI
  → Mesa Venus (fork)      guest 侧 Vulkan 驱动；ring 同步、fence 等待、内存发布都有定制
  → vtest socket           私有 present 命令（mesa 与 virglrenderer 两端成对改）
  → virglrenderer (fork)   宿主侧；shadow 内存桥、present 回调、fence 缺陷兜底
  → Maleoon Vulkan 驱动    宿主 GPU（不改，只兜缺陷）
  → OHNativeWindow         送到鸿蒙合成器
```

OpenGL 帧（Wine GL / D3D9 走 wined3d）类似，guest 侧换成 Mesa virpipe，host 侧走 virglrenderer 的 VirGL 路径。

D3D12 走 vkd3d-proton，**上游原样未改**——descriptor 堆上限、内存同步档位这些适配都做在环境变量注入层（`entry/src/main/cpp/wine/`），不在 vkd3d 代码里。

## 模块一览

定制程度分四级：**重度 fork**（核心功能改动）、**轻度 fork**（少量功能改动）、**仅构建适配**（改构建不改功能）、**原样引入**（无任何提交）。

| 模块 | 基线 | 角色 | 定制程度 | 文档 |
|---|---|---|---|---|
| wine | wine-11.10 | Windows API 实现 | 重度 fork | [wine.md](wine.md) |
| box64 | v0.4.3 | x86_64→ARM64 转译器 | 重度 fork | [box64.md](box64.md) |
| virglrenderer | 上游 main | 宿主侧图形服务（Venus + VirGL） | 重度 fork | [virglrenderer.md](virglrenderer.md) |
| mesa | OpenHarmony-v6.0-Beta1 | guest 侧 GL（virpipe）+ Vulkan（venus）驱动 | 重度 fork | [mesa.md](mesa.md) |
| dxvk | v1.10.3 | D3D9/10/11→Vulkan（legacy 档） | 重度 fork | [dxvk-legacy.md](dxvk-legacy.md) |
| dxvk-modern | v2.6.2 | D3D9/11→Vulkan（modern 档） | 重度 fork | [dxvk-modern.md](dxvk-modern.md) |
| libepoxy | 1.5.10 | EGL/GLES 函数加载（virglrenderer 依赖） | 轻度 fork | [libepoxy.md](libepoxy.md) |
| glib | 2.78.0 | gstreamer 依赖 | 仅构建适配 | [build-adaptations.md](build-adaptations.md) |
| gstreamer | 1.24.4 | Wine 媒体管线后端 | 仅构建适配 | [build-adaptations.md](build-adaptations.md) |
| pcre2 | 10.42 | glib 依赖 | 原样引入* | [build-adaptations.md](build-adaptations.md) |
| wayland / wayland-protocols | 1.22.0 / 1.39 | winewayland.drv 构建依赖（协议扫描） | 原样引入 | — |
| vkd3d-proton | 2.6 | D3D12→Vulkan | 原样引入 | — |
| freetype | 2.13.3 | Wine 字体光栅化 | 原样引入 | — |
| libxkbcommon / xkeyboard-config | 1.7.0 / 2.46 | 键盘映射（winewayland 输入） | 原样引入 | — |
| libdrm | OpenHarmony-v6.0 | mesa venus/virgl 构建依赖 | 原样引入 | — |
| libxml2 | 2.12.0 | Wine 构建依赖 | 原样引入 | — |
| libffi | 3.4.6 | glib 依赖 | 原样引入 | — |
| gnutls / nettle / libtasn1 / libunistring / gmp | 3.8.3 系 | Wine schannel 的 TLS 链 | 原样引入 | [build-adaptations.md](build-adaptations.md) |

\* pcre2 工作区有一处 autoconf 重新生成的注释引号差异，无实质改动。

mesa 和 libdrm 用的是 OpenHarmony 官方分支而不是上游 mesa，为的是跟鸿蒙的构建方式一致；定制提交叠加在这个分支上。

## 定制点地图（按平台问题域）

每个定制点：问题 → 思路。详情看对应模块文档。

### 进程模型：没有 execve 的世界里怎么起进程

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| wineserver 由宿主拉起 | wineserver 是 `.so` 不能 posix_spawn → ntdll 走 Process Broker 请求宿主创建，socket 扫描 + 就绪轮询防重复 | wine |
| wine 子进程转发 | NCP 模式不支持嵌套 fork → `spawn_process` 全部转发宿主 broker | wine |
| box64 共享库模式 | box64 不能作为可执行文件运行 → 编成 `box64.so`，宿主 wine 子进程 dlopen 后调 `box64_hmos_main` | box64 |
| vtest 服务共享库化 | vtest server 也不能独立起进程 → 封装成 `libwinehua_vtest_server.so` 由宿主 dlopen，并注入 present 回调 | virglrenderer |

### 内存与执行权限：noexec 分区上跑 JIT 和 PE

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| 匿名 RWX 拒绝 | 内核对匿名 RWX mmap 返回 EPERM → 先 RW 映射再 `mprotect` 加 X（box64 再配合 `prctl(0x6a6974)` 打开内核 JIT 白名单） | wine、box64 |
| noexec 文件映射 | 文件映射 + `PROT_EXEC` 被拒 → 匿名映射 + pread 读入内容 | box64 |
| PE 执行段 | 页权限创建时确定，`mprotect` 假成功 → ntdll 的段映射委托 `ohos_map_exec_section`（匿名 RWX） | wine |
| 低 4GB 地址 | 32 位 guest 指针必须 <4GB，而内核忽略低地址 hint → 解析 `/proc/self/maps` 找空洞，MAP_FIXED 精确抢占 | box64 |

### libc：musl 缺什么补什么

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| glibc 私有符号 | fts/obstack/error/`__ctype_*_loc`/qsort_r 等不存在 → 从 NetBSD/glibc 移植或写 weak stub | box64 |
| dlopen 语义差异 | musl 无 `RTLD_NEXT` 链、`dlopen(NULL)` 拿不到自身符号 → 改 `dladdr` 自定位、RTLD_DEFAULT | box64、wine |
| epoll_pwait2 | musl 老版本没有 → wineserver 里加 ENOSYS 弱符号 stub | wine |
| 中文 locale | musl locale 解析缺位导致中文 UI 退化 → ntdll 补 LC_ALL 解析 + 构建产物编入 po 翻译 | wine、构建 |

### 文件系统：没有 symlink 的盘符布局

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| 盘符映射 | `dosdevices/c:` 建不出来 → `ohos_drive_unix_path()` 硬编码映射（Z:→Download、C:→prefix），server 侧再建命名事件对象让 `GetLogicalDrives` 能枚举 | wine |
| 路径布局 | 打包路径与发行版不同 → `WINEDATADIR`/`WINEBINDIR`/`WINEUNIXDIR` 环境变量覆盖 data/bin/dll 目录 | wine |
| DLL 搜索 | WoW64 下 exe 旁的原生 DLL 找不到、DXVK overlay 要按档位注入 → loader 三处搜索路径扩展 | wine |

### 图形呈现：画面怎么送到 OHNativeWindow

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| 私有 swapchain | 宿主只有 OHNativeWindow，没有 Vulkan 原生 WSI → surface/swapchain 全实现在 win32u，句柄编码 Wayland proxy id，present 经 `vn_winehua_present` 交给宿主 | wine |
| vtest present 命令 | guest 与宿主分处两进程，帧要显式推送 → 两条私有 vtest 命令（GL 路径 0x57485052 / VK 路径 0x57485650），带 Venus 对象 id 与显示 deadline | mesa + virglrenderer |
| present 回调桥 | 宿主收到 present 命令后要送到具体窗口 → virglrenderer 暴露回调注册接口，宿主 app dlopen 时注入；present 与 QueueSubmit 共享 vk_mutex 保证顺序 | virglrenderer |
| GL 前缓冲 | Wine GL 双缓冲应用的 flush 收不到 → mesa 在 `WINEHUA_VTEST_PRESENT` 下强制走完整 frontbuffer flush 路径 | mesa |
| wayland 窗口语义 | 任务栏/桌面壳要被宿主识别、窗口 min/max 要生效 → app_id 后缀（`.taskbar`/`.desktop-shell`）、桌面模式转发真实屏幕坐标、min/max 扩展 | wine |

### Venus/Maleoon：缺特性兜缺陷

宿主 Vulkan 走 Venus 虚拟化，特性缺失和驱动缺陷都不少。这是 fork 里行数最多的一类：

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| shadow 内存桥 | Maleoon 无 dma-buf 外部内存导出，guest/host 无法共享同一块 Vulkan 内存 → host 建匿名 shadow 文件给 guest 映射，submit 时机做 shadow↔host 双向拷贝与缓存域转换 | virglrenderer |
| mapped flush 协议 | shadow 下 CPU 写必须显式发布 → DXVK 的 Map/Unmap 链路补 `vkFlushMappedMemoryRanges`，按 (memory, offset) 合并区间批处理 | dxvk 两版 |
| fence 缺陷 | Maleoon `GetFenceStatus` 瞬态返回 OOM 会毒化 ring → 重试 ≤4 次后降级为 `WaitForFences(timeout=0)` 等价查询 | virglrenderer |
| bool spec 常量 | 驱动编译器错误处理 bool 特化常量 → SPIR-V 二进制上把 `OpSpecConstantTrue/False` 烘焙成普通常量 | dxvk 两版、virglrenderer |
| BC 纹理压缩 | Venus 无 `textureCompressionBC` → 上传时 CPU 解压 BC1-BC7（复用 wine 的 bcdec.h），DXGI 格式表重映射 | dxvk 两版 |
| 双源混合 | Venus 无 `dualSrcBlend` → 严格条件下拆两遍 draw 模拟，五种可切换模式 | dxvk legacy |
| 单采样 A2C 透明片元 | Maleoon 单采样 alpha-to-coverage 不剔除全透明片元（树叶/围栏成方块）→ FS 输出后按 spec constant 门控 `opKill`（Maleoon 默认开） | dxvk 两版 |
| instance divisor 缺失 | Venus 无 `vertexAttributeInstanceRateDivisor`，instancing 只取首顶点 → CPU 展开 per-instance 顶点，Sha1 缓存 | dxvk legacy |
| present 颜色空间 | 源 sRGB、目标非 sRGB 时 blit 线性拷贝偏色 → present shader 内按 push constant 做线性→sRGB 编码 | dxvk 两版 |
| cube array shadow | 原生指令会挂死 host ring → 声明期降为 2D-array，shader 内做坐标转换 | dxvk legacy |
| custom border color | 无 `VK_EXT_custom_border_color` → cb15 UBO 传参，采样后按权重混入 border 色 | dxvk legacy |
| 特性检测放宽 | Venus 缺一批核心特性，上游 2.6.2 直接拒绝建设备 → fork 带整套兼容实现；2.6 上还有与 vkd3d-proton 2.6 swapchain factory 的版本错配修复 | dxvk 两版 |
| ring 同步 | shadow 映射下宿主写的 IDLE 状态 guest 读不到、tail 发布存在弱内存序问题 → 强制通知开关 + seq_cst fence 选项 | mesa |
| fence 等待 | fence feedback 不可用，轮询退化成跨 socket 轮询 → timeline 事件等待 / 直连等待 / 上游轮询三模式 | mesa |
| 提交前内存发布 | VKD3D 上传堆长期保持映射，没有 Unmap 边界 → device 追踪 coherent 映射，QueueSubmit 前经 Venus 协议发布 | mesa |

### 音频：沙箱里出声

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| ohos 音频后端 | 沙箱不能调 OHOS 音频 API → 新增 wineohos.drv，Unix socket + mmap 环形缓冲把 PCM 交给宿主混音（固定 48kHz s16） | wine |
| MCI 媒体播放 | DShow 管线在 OHOS 不可用 → mciqtz 加 waveOut 后端（minimp3 软解），DShow 失败自动回退 | wine |
| 格式协商 | 后端固定 48kHz，44.1kHz 客户端被拒 → 按驱动名识别，后端内部归一化 | wine |

### 窗口与输入

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| 装饰窗受管判定 | 程序自绘阴影窗（owner+三件套 exstyle）被判成独立窗口，多出黑窗 → `is_window_managed` 对带 owner 的纯装饰浮层返回 FALSE，走上游既有的 subsurface 通路 | wine |
| 桌面模式窗口 | 虚拟桌面模式下所有窗口（含桌面壳）都要 subsurface 根 → 桌面模式跳过"仅父窗口可见"过滤 | wine |
| 模态对话框上报 | 宿主合成器需要模态关系来编排层级与输入路由 → wine 探测后经私有 `winehua-toplevel` 协议上报 | wine |
| 标题栏按钮 | msstyles 主题字体顶掉系统中文；高 DPI 下按钮成窄高矩形 → 删主题字体行；defwnd.c 尺寸统一函数，绘制与 hit test 共用 | wine |
| 手柄转发 | 无 evdev/libinput 手柄生态 → winebus 走宿主 socket（XInput/DirectInput/rumble），显式开启 | wine |
| 相对指针 | 游戏需要原始鼠标运动 → winewayland 接 zwp_relative_pointer_v1，wine 侧按需求决定是否进入相对模式 | wine |

### 网络

| 定制点 | 问题 → 思路 | 模块 |
|---|---|---|
| TLS 链引入 | schannel 依赖 gnutls，系统没有 → 交叉编译整条 gnutls 依赖链（nettle/tasn1/unistring/gmp）随包分发 | 构建 |
| guest resolver 符号 | box64 wrappedlibc 缺 musl resolver 符号，guest TLS 握手失败 → 补 7 个符号 | box64 |
| DNS 解析 | musl 无 libresolv，dnsapi 的 resolver 调用无着落 → 内建 musl 移植实现（libresolv_musl.c） | wine |
| Windows 版本伪装 | 程序按版本分支行为，固定伪装不够 → `WINEHUA_WINDOWS_VERSION` 按进程覆盖（CrossOver 移植） | wine |

## 维护约定

- **定制必须留痕**：所有 fork 的提交以 `winehua:`/`fix(venus):` 等前缀与上游提交区分；各模块的逐文件明细（合并上游时的对照清单）在本目录各篇的"补丁清单"部分。
- **默认关闭、按需开启**：绝大多数行为定制由环境变量门控（`WINEHUA_*`/`VKR_WINEHUA_*`/`VN_WINEHUA_*`/`DXVK_WINEHUA_*`），默认关闭时行为与上游一致。环境变量全集见各模块文档。
- **协议两端成对改**：vtest 私有命令号、字段序、版本号在 mesa 与 virglrenderer 两侧必须同步演进，任一侧漂移即 `-EPROTO`。协议清单见 [architecture/contracts.md](../architecture/contracts.md)。
- 升级/合并上游的操作流程见 [../assets/submodule-maintainability.md](../assets/submodule-maintainability.md)。
