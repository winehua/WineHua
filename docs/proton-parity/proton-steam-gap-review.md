# 从架构看：WineHua 距离「跑 Proton / 跑 Steam」还差什么

> 整理日期：2026-09-12
> 分支：`feature/proton-arm64-parity`（基线 `feature/arm64-heaven-port @ 2728523`）
> 用途：交给专家评审"架构缺口清单"与"优先级"。
> **视角说明：本文是架构分析，不是运行状况汇报。** 少量设备观察只作为"该路径确实被走到过"的旁证，
> 不构成"现在能/不能跑"的结论。

---

## 0. 先厘清三个层级（否则口径会打架）

"跑 Proton"和"跑 Steam"是两件不同的事，中间差了整整一层产品集成：

| 层 | 是什么 | 在我们项目里的角色 |
| --- | --- | --- |
| **Proton** | Valve 的**兼容层配方**：Wine + DXVK / vkd3d-proton + FEX（ARM 上）+ 启动器脚本（pressure-vessel / Python）。它本身不是可执行物，也不对最终用户暴露 | **对齐对象**：我们要在 ARM64 上把它的核心组件与接口契约拿过来 |
| **Windows Steam 客户端** | 要跑的那个应用：`steam.exe`(32 位) + `steamwebhelper.exe`(64 位, Chromium/CEF) + 一堆辅助进程 | **被移植的目标应用** |
| **WineHua ARM64 运行时** | 我们做的东西：aarch64 Wine + FEX CPU DLL + UnixLib + 图形栈 + OHOS 平台层 | **承载者** |

**架构上的因果关系是：**

```text
Steam(Windows 客户端)  ──需要──▶  Windows NT 语义 + x86/x64 指令执行
                                        │
                                        ├─ WineHua ARM64 运行时提供
                                        │    (aarch64 Wine + FEX + OHOS 平台层)
                                        │
                                        └─ 而"按 Proton 的组件与接口对齐"
                                             是为了让这套运行时在 ARM 上够快、够兼容
```

所以本文的结论会落在两类缺口上：

- **§2 型缺口（Proton 对齐侧）**：运行时本身的架构/接口/构建是否与 Proton 的 ARM 路线一致；
- **§1 型缺口（Steam 集成侧）**：Steam 这个具体应用对宿主提出的要求（进程、IPC、窗口、注入…），
  与"能跑普通单进程 Windows 程序"根本不是一回事。

方案 §6.3 已经明确：**不移植 pressure-vessel 与 Steam 的 Python 启动层**（那是 Linux Steam 的机制），
但 prefix 创建、架构 DLL 部署、库搜索路径、资源版本等**职责**必须由 WineHua 平台层承担。

---

## 1. 需求侧：Steam 客户端对宿主运行时提出了哪些架构要求

按子系统拆。每一项都标注**为什么 Steam 特别在意**（普通单进程小游戏不一定在意）。

| 子系统 | Steam 的具体要求 | 与"普通单进程 Windows 程序"的差别 |
| --- | --- | --- |
| **1.1 多进程** | 主客户端 + `steamwebhelper`(CEF) + 崩溃上报 + 商店/云同步辅助进程，进程数上双 | 普通程序常常只有一个进程 |
| **1.2 32/64 混合** | `steam.exe` 是 32 位，`steamwebhelper` 是 64 位，**必须在同一 prefix 里同时活着并互相 IPC** | 单进程程序只用到一种位数 |
| **1.3 进程间通信** | 命名管道、loopback TCP（`steamloopback.host`）、共享内存、Chromium 的 Mojo | 单进程程序几乎不用 |
| **1.4 文件系统** | 超大安装量、大小写不敏感查找、长路径、符号链接语义、大文件 mmap | 经典 Windows 程序的路径假设更宽松 |
| **1.5 窗口与图形** | CEF 会创建**自己的 Win32 子窗口**并自己算几何；UI 走软件光栅（`--disable-gpu`）或 GPU 合成 | 普通程序通常一个主窗口 |
| **1.6 网络与安全** | 到 Steam 后端的 TLS（自带的 BoringSSL/OpenSSL）+ 证书校验 + DNS + UDP | 单机程序不需要 |
| **1.7 注入 / Overlay** | `GameOverlayRenderer.dll` 要**注入到游戏进程**里 hook 图形 API | 与"跑起来"无关，但 Steam 默认启用 |
| **1.8 由 Steam 启动游戏** | 游戏**不是**我们手动拉起的，而是 Steam 用自己的一套 env + 继承句柄 + 自己的 DLL 去 `CreateProcess` | 这条路径与"手动跑一个 exe"完全不同 |

---

## 2. 供给侧：WineHua 架构现状与逐项映射

### 2.1 现有平台架构（一张图看清责任边界）

```text
┌─ App 进程 (ArkTS + libentry.so) ────────────────────────────────────┐
│  UI / WineWindow* / 输入采集 / 音频采集                              │
│  WineEnvService : 会话环境、DXVK 档位、启动引擎                      │
│  GameHook/SmokeHook : Want 参数驱动的自动化入口                      │
└──────────┬──────────────────────────────────────────────────────────┘
           │ NCP (OH_Ability_StartNativeChildProcess)
           ▼
┌─ wine_child.so (每 Wine 进程一个 NCP 子进程) ────────────────────────┐
│  ntdll.so (Unix 侧) ── wineserver (另一个 NCP 进程, unix socket)     │
│  CPU 后端: x64→libarm64ecfex.dll / x86→libwow64fex.dll 或 wowbox64   │
│  UnixLib : libarm64ecfex.so / libwow64fex.so (TSO/原子/SHM)          │
│  OHOS 适配: ohos_broker.c(进程) ohos_file.c(文件) ohos_virtual.c(信号)│
└──────────┬──────────────────────────────────────────────────────────┘
           │ vtest socket
           ▼
┌─ virgl_child.so (NCP) 宿主图形 ─────────────────────────────────────┐
│  virglrenderer + vkr → SurfaceQueue → egl → XComponent              │
│  嵌入式 Wayland compositor (wl_core/xdg_shell/compositor/*)          │
└─────────────────────────────────────────────────────────────────────┘
```

平台层与 Wine 的分界写在 `thirdparty/wine/dlls/ntdll/unix/ohos_{broker,file,virtual}.c`
三个文件里，进程创建走 `entry/src/main/cpp/proc/broker.cpp`。

### 2.2 逐项对照

> 结论列取值：**在位**（架构上具备）/ **在位待验**（机制存在但没针对该场景验证）/ **缺口**（架构上没有）

| # | Steam 要求 | WineHua 现有架构 | 结论 |
| --- | --- | --- | --- |
| 2.2.1 多进程 | Wine `CreateProcess` → `broker.cpp` → NCP 子进程；`ohos_broker.h` 定义 SPAWN 协议（unix socket + SCM_RIGHTS 传命名 FD，**上限 16 个**） | 机制完整 | **在位待验**：CEF 的进程数与句柄需求需要实测；16 FD 上限要核对够不够 |
| 2.2.2 32/64 混合 | 两个 CPU DLL（`libwow64fex.dll` / `libarm64ecfex.dll`）+ aarch64 Wine 的 WoW64；同一 prefix 内 32/64 共存 | 架构在位 | **在位** |
| 2.2.3 命名管道 | 由 wineserver 实现（NT 语义），传输是到 wineserver 的 unix socket | 架构在位 | **在位待验** |
| 2.2.4 loopback socket | 走 OHOS 内核网络栈；`steamloopback.host` 只是 hosts 解析 | 架构在位 | **在位待验**（DNS/hosts 行为要确认） |
| 2.2.5 共享内存 | `shm_open` 可用（FEX 统计已在用）；Wine 的 section 走 mmap | 架构在位 | **在位待验**：Chromium 的 shared memory / Mojo 路径未验证 |
| 2.2.6 文件语义 | `ohos_file.c` 适配；**HAP 不支持 symlink，dosdevices 走四条硬编码 fallback**；noexec 文件系统用「匿名 mmap + pread」替代可执行段映射 | 机制存在，但有**结构性妥协** | **缺口（结构性）**：Steam 依赖符号链接语义与大量 mmap，需评估 fallout |
| 2.2.7 窗口与几何 | 嵌入式 Wayland compositor + `winewayland.drv`，支持 xdg_toplevel / subsurface / popup | 架构在位 | **在位待验**：CEF 用原生 Win32 子窗口 + 自定义几何（占位坐标 `0x30000000` 这类值需要规范化），这条要专门核对 |
| 2.2.8 UI 软件光栅 | WineD3D（→OpenGL→virgl）+ GDI | 架构在位 | **在位** |
| 2.2.9 文本/字体 | Wine 自带字体 + 打包资源 | 架构在位 | **在位待验**：CEF 的 DWrite/字体回退在中文字形上要单独看 |
| 2.2.10 TLS / 证书 | 依赖 Wine 的 `crypt32` + 证书导入 | 机制在 Wine 侧 | **在位待验**：Steam 登录是 TLS，必须实测证书链 |
| 2.2.11 Overlay 注入 | **没有对应机制** | —— | **缺口（架构）**：ARM64EC / ARM64X 边界上的 DLL 注入与 API hook 需要单独设计 |
| 2.2.12 由 Steam 启动游戏 | 走 Wine 正常 `CreateProcess` → broker → NCP（不经过 `winehua.mode=game` 自动化入口） | 架构在位 | **在位待验**：这条路径与"手动跑 exe"不同，要单独验证 |

---

## 3. 缺口清单（按架构影响排序）

| 序 | 级别 | 缺口 | 归属层 | 为什么是架构问题 | 建议 |
| --- | --- | --- | --- | --- | --- |
| 1 | **G0** | **文件系统语义妥协**：HAP 无符号链接 → dosdevices 硬编码 fallback；noexec → 可执行段"读出来再匿名映射" | Wine OHOS 平台层（`ohos_file.c` + 打包层） | Steam 的安装/更新/库发现都假设完整路径语义与符号链接；这不是"某个游戏的问题"，是**所有依赖该语义的应用的结构性风险** | 先做一次"路径语义矩阵"：stat/lstat/readlink/open/mmap 在 dosdevices、Z:、C:\ 三类路径上的实际行为，与 Linux Wine 逐项对照 |
| 2 | **G0** | **窗口几何规范化缺位**（CEF 子窗口） | Wine `win32u` + `winewayland.drv` + 我们的 compositor | CEF 自己算几何并创建子窗口，宿主必须正确处理"未初始化/超范围坐标"。这决定了 **Steam UI 能不能出现在屏幕上** | 明确"谁负责把占位坐标归一"（win32u 的 window placement vs wayland 驱动 vs compositor），并和上游行为对齐 |
| 3 | **G1** | **Chromium/CEF 的 IPC 与共享内存未验** | Wine OHOS 平台层（管道/共享内存/socket）+ Steam 自身参数 | CEF 是 Chromium：Mojo、shared memory、多进程。这是"Steam 能不能稳定"的核心路径 | 用单进程 CEF（Steam 已带 `-cef-single-process`）与非单进程两种模式各走一遍，确认 IPC 面 |
| 4 | **G1** | **Overlay 注入无机制** | 平台 + 图形层 | 在 ARM64EC（x64 转译）与 ARM64X（跨边界调用）下注入并 hook 图形 API，是独立课题 | 明确"首版不支持 Overlay"是否可接受；若要支持，需要单独设计（而不是塞进现有链路） |
| 5 | **G1** | **broker 的 16 FD 上限 / 进程模型未针对 CEF 校验** | `ohos_broker.h` + `broker.cpp` | 进程与句柄继承是 NT 语义的地基，CEF 会放大这块的压力 | 先用一个"多进程 + 多句柄"的最小测试程序压一遍 broker |
| 6 | **G2** | **TLS / 证书链未验** | Wine `crypt32` | 登录必须过 | 直接测 Steam 登录流程 |
| 7 | **G2** | **字体 / DWrite 中文字形未验** | Wine + 打包资源 | 影响可用性不影响启动 | 与 UI 一起测 |

---

## 4. Proton 对齐在这套架构里的位置

把 §2.2 的"在位"项和 §3 的缺口对照可以看出**责任分布**：

| 层 | 状态 | 说明 |
| --- | --- | --- |
| **Proton 对齐侧**（Wine aarch64 + FEX CPU DLL + 成套 UnixLib + ARM64EC/ARM64X 图形库） | **基本到位** | 已按 Proton 的 ARM 路线把组件与接口补齐，并有真机证据（UnixLib 被加载、SHM 统计可读、x64+ARM64X 图形库确实被加载）。详见 `p2-*.md`、`p3-p4-smoke-ab.md` |
| **Steam 集成侧**（进程/IPC/文件/窗口/注入） | **主要缺口所在** | 见 §1 与 §3，全部属于"应用对宿主的要求"，与"ARM 指令对齐"不是一回事 |

**所以架构上的判断是**：把 Proton 的 ARM 组件对齐做完，解决的是"**这套运行时能不能高效执行 Windows 程序**"；
而要"**跑 Steam**"，主要工作量在 §3 那几条应用集成路径上，尤其是**文件语义（1）**与**窗口几何（2）**。
这两条不解决，Steam 即使进程全起来了，也可能看不到界面或装不了游戏。

另外有一条边界要写清楚：**Proton 的启动器层（pressure-vessel / Python）在本架构里没有对应物，
也不需要**——Steam 的 Windows 客户端自己就是被跑的那一方，它的启动逻辑走 Wine 的 `CreateProcess`。

---

## 5. 建议向评审专家提的问题

1. **文件语义**：HAP 不支持符号链接这一条，业界在 HarmonyOS/Android 上跑 Steam 类应用时通常怎么处理
   （在 prefix 内建一层映射？还是接受 fallback 并逐个修应用假设）？有没有已知的成功先例？
2. **窗口几何**：CEF 传入 `0x30000000` 这类占位坐标时，标准 Wine（X11/Wayland 驱动）是由谁归一的？
   我们的 compositor 应该对齐到哪一层（驱动 vs 合成器）？
3. **Overlay**：首版是否可以明确"不支持 Steam Overlay"？若必须支持，在 ARM64EC 转译进程里注入的可行路径是什么？
4. **优先级**：专家是否同意"先解决文件语义与窗口几何，再谈 Proton 组件版本整体回移"？
5. **验收口径**：专家建议用哪一组"最小可用判据"来定义"Steam 可用了"
   （例如：能登录 → 能进库 → 能下载 → 能启动一款游戏并出画面），我们按这个口径排工作。

---

## 6. 相关文档

| 主题 | 文档 |
| --- | --- |
| 当前架构与各层改动 | `architecture-overview.md` |
| Proton 对齐的接口/构建/配置证据 | `wine-fex-interface-audit.md`、`fex-build-parity.md`、`p2-*.md` |
| 性能结论（哪一段是瓶颈） | `p3-p4-smoke-ab.md` |
| 归因与回退策略 | `p5-merge-decision.md` |
| 锁定参考值（Proton / FEX / DXVK / vkd3d） | `proton-arm64-parity.lock.yaml` |
| 现有平台架构（四域总览） | 仓库 `docs/ARCHITECTURE_OVERVIEW.md`、`docs/ARCHITECTURE.md` |
