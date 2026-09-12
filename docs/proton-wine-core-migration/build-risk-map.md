# 换 Wine Core 的构建风险地图（W0 → W1 输入）

> 目的：W1 动手前先知道**哪里会炸、按什么顺序做、什么条件下停手**。
> 依据：`wine-core-delta.md` 的对照结果 + 现有构建链（`scripts/build_wine.sh`、
> Docker `winehua-dev`、llvm-mingw 20260826、OHOS command-line-tools 6.1.1.290）。

## 1. 风险按层排列（从高到低）

| 层 | 风险 | 为什么 | 触发后果 |
| --- | --- | --- | --- |
| 构建系统 | **很高** | 我们的构建是 `scripts/build_wine.sh` 手工编排（native tools → unix so → PE DLL → wineserver 两套架构），Valve 走 Proton 的 Makefile/configure.sh | 产物不齐或混搭；loader/wineserver/ntdll 不成套 |
| WoW64 / ARM64EC 接口 | **很高** | Valve 树是 Wine **11.0**，我们是 **11.10**；`dlls/wow64/*`、`signal_arm64ec.c`、`ndr_stubless.c` 这一年都改过 | 地址空间策略写错 → 随机崩溃、难归因 |
| 进程与 server 协议 | 高 | `server/*` 与 Wine 版本强绑定；我们改过 `process.c` 与 wineserver 生命周期 | wineserver 与 Wine 版本不匹配 → 起不来 |
| 文件语义 | 中高 | OHOS 无 `symlink()`，dosdevices 回退链是我们自造的 | 盘符枚举 / 安装 / 重命名失败 |
| 图形（win32u / winewayland） | 中高 | 我们改了上游文件，且与 HAP 侧 compositor 协议耦合 | 窗口不上屏、几何与输入错位 |
| 音频 / 输入 | 中 | `wineohos.drv` 与宿主 ABI 绑定，两端要同步 | 无声音、手柄失效（不致命） |
| 诊断 / 清理类 | 无 | 不迁 | — |

## 2. 两个基线的构建差异（W1 先要解决）

| 项 | 当前 WineHua | Valve Proton Wine |
| --- | --- | --- |
| 目标 | OHOS aarch64（HarmonyOS NCP 子进程内运行） | Linux（Proton Runtime），另有 ARM64 实验线 |
| 构建入口 | `scripts/build_wine.sh`（宿主机脚本 + Docker） | Proton Makefile + configure.sh |
| 工具链 | llvm-mingw 20260826（PE）+ OHOS clang（unix so）+ musl 兼容层 | Proton Container 里的 GCC/Clang + glibc |
| 必须保留的产物 | ntdll.so、wineserver（unix）、PE DLL 双架构产物 | 仅 Linux 产物 |

结论：W1 不可能直接套用 Valve 的构建系统，只能是**沿用我们自己的 build_wine.sh、
把 wine 源码目录指向 Valve 基线**，再逐个修构建失败点。
这决定了 W-19（构建系统）必须是 W1 的**第一步**，而不是最后一步。

## 3. 与 Proton ARM64 / WoW64 / FEX 正式接口的冲突风险点

对应方案 §14 第 4 问，逐条回答：

| 风险点 | 冲突性质 | 处理 |
| --- | --- | --- |
| ARM64EC dispatch / rpcrt4 thunk（W-21） | **不是冲突，是共有的上游缺陷**：Valve 树里该段与我们逐字相同 | 我们的 patch 直接带过去；换基线不会自动修好 |
| WoW64 `HODLL` 选择（W-06） | 我们让 ARM64 默认走 `libwow64fex.dll`；Valve 的 wow64 假定 Linux/glibc | 必须保留我们 unix 层的 HODLL 逻辑，不能照抄 Valve |
| FEX UnixLib（W-20） | Valve 的 FEX pin 是 `1cc4b93e`（FEX-2607），我们是 `86ff33bbe` + 6 提交回移 | 首轮固定不动；命名上不得称"与上游一致" |
| wineserver 生命周期（W-02） | Valve 假定 wineserver 由 Linux 正常 fork/exec 管理 | broker 注入必须保留 |
| unix 系统调用面：noexec / SMC（W-04） | Valve 假定文件系统可 `PROT_EXEC` | 必须保留 noexec / JIT 路径 |
| prefix 与 Steam 集成 | Valve 依赖 pressure-vessel / Linux Steam Runtime | 方案明确不引入，保持我们的 prefix 与 NCP |

## 4. 最小可启动 candidate：三档 patch 集

原则：**先能起，再对齐，再补全**。每档都必须独立可验收、可回退。

### M1 — 能起 wineboot / cmd（预计改动最小）

```text
W-19（构建系统，最小集）  → 让 Valve Wine 能在 build_wine.sh 下编出 ntdll.so / wineserver / ntdll PE
W-05（loader / env 基建） → 路径推算、DLL 搜索、__OHOS__ 守卫
W-01 + W-02（broker/NCP + wineserver 生命周期）
W-03（filesystem 最小回退）
W-22（wineboot，如需）
```

Gate：`wineboot` 建 prefix 成功；`cmd.exe` 有输出；反复 5 次冷启动稳定。

### M2 — 32/64 与图形可用

```text
M1 + W-06（WoW64 / 地址空间）+ W-04（noexec / SMC / signal）
   + W-08 + W-09（win32u / winewayland）+ W-12（字体）
```

Gate：x86 hello / x64 hello 单独与互相 CreateProcess；FEX 两种后端真实生效（非静默回退）；
notepad / 基础 Win32 窗口 / 弹窗 / 多窗口；geometry 与输入命中正确。

### M3 — 完整平台能力

```text
M2 + W-07（ARM64X overlay）+ W-10（音频）+ W-11（输入）
   + W-13（dnsapi）+ W-16（keep-alive）+ W-21（rpcrt4 ARM64EC）
```

Gate：network / TLS（DNS/TCP/TLS/WinHTTP/WinInet/crypt32 证书链）；
多进程 IPC（父子 32/64、命名管道、共享内存、loopback、句柄继承）；
音频与手柄冒烟。**M3 通过后才进入 S0（Steam 启动）。**

## 5. Gate 清单（对应方案 §9）

| Gate | 内容 | 用现有资产怎么测 |
| --- | --- | --- |
| W0 基础 | wineboot / cmd / reg | 直接跑 |
| W1 32/64 WoW64 | x86 hello、x64 hello、x86→x64、x64→x86 | 需要补一对 hello 工程（目前没有） |
| W2 FEX | 两种后端真实生效、UnixLib、SHM stats、不得静默 fallback | 复用 `p2-device-validation.md` 的判据 |
| W3 GUI | notepad、窗口/弹窗/多窗口、geometry、输入 | 复用 `proton-steam-gap-review.md` 的窗口检查表 |
| W4 网络/TLS | DNS/TCP/TLS/WinHTTP/WinInet/crypt32 | 需补最小 TLS 用例（目前没有） |
| W5 多进程/IPC | 父子 32/64、命名管道、共享内存、loopback、继承句柄 | `comprobe.exe` 已覆盖 RPC/服务一半；需补 pipe/SHM |

## 6. 止损点（出现以下情况就停下汇报，不硬推）

1. M1 的构建系统在两天工作量内无法产出成套 loader/wineserver/ntdll。
2. 发现必须新写 wineserver 协议才能对齐 → 停下来评估收益。
3. M2 的 WoW64 地址空间策略出现"改了 A 崩 B"的循环，且无法用 comprobe/smoke 判定。
4. 迁移中发现 Proton Wine 的某块实现（如 ARM64EC）比我们的 11.10 基线更旧，
   必须回移上游 → 该块改为 `PREFER_UPSTREAM`（留在 11.10 分叉上）而不是硬迁。

## 7. 本文件边界

- 三档划分是**规划**，不是实测；每档实际工作量要在 W1 里用"能否过 Gate"验证。
- 尚未尝试实际编译 Valve Wine；M1 的第一个真实动作就是把它编起来，
  预期会暴露一批 configure / 头文件 / 工具链问题。
