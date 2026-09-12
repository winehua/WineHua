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

## 10. W1 第二次实做：configure 通过、make 编完近整棵树（同日后续）

方案 A（补齐 out-of-tree 流程）**其实能走通**，关键是补上「上游 autogen.sh 的生成步骤」：

```text
autoconf                          OK
make_requests / make_specfiles    OK   （产出 ntscalls 类生成物：ntsyscalls.h、server_protocol.h 等）
make_vulkan                       OK   （产出 include/wine/vulkan.h + winevulkan thunks）
configure                         OK   "configure: Finished.  Do 'make' to compile Wine."
make                              OK   运行到 98549 行日志（几乎编完整棵树）
```

这一轮又修掉 6 个构建阻塞，全部属于 W-19：

| # | 现象 | 根因 | 处置 |
| --- | --- | --- | --- |
| 4 | `make: *** No rule to make target '<srcdir>/configure'` | out-of-tree 时 `config.status` 依赖 `$(srcdir)/configure`，Valve 树里没有（上游 autogen.sh 会在树内生成） | 构建脚本：缺则从构建目录拷回源码树 |
| 5 | `dlls/ntdll/unix/loader.c: 'jni.h' file not found` | Valve 11.0 的 loader.c 带 `#ifdef __ANDROID__` 的 **JNI 入口**（`JNI_OnLoad`/`wine_init_jni`）；我们的 CFLAGS 定义了 `__ANDROID__`（musl 路径需要） | 源码补丁：改成 `#if defined(__ANDROID__) && !defined(__OHOS__)`，OHOS 侧跳过 |
| 6 | `dlls/amd_ags_x64/unixlib.c: 'amdgpu_drm.h' file not found` | 该模块直接 `#include <amdgpu_drm.h>`，而 sysroot-ext 的头在 `usr/include/libdrm/`；此模块我们 fork 里没有（11.0→11.10 上游新增） | CFLAGS 加 `-I$SYSROOT_EXT_INC/libdrm` |
| 7 | `'M_PERTURB' undeclared`；随后 `'amdgpu.h' not found` | `M_PERTURB` 是 glibc 的 malloc 调试开关，musl 没有；`amdgpu.h`/`libdrm_amdgpu` 我们没建 | 源码补丁：`mallopt(M_PERTURB,…)` 用 `#if defined(M_PERTURB)` 包住；`amd_ags_x64` 在 configure.ac 里先排除（AMD 专用 shim，OHOS/Mali 无意义） |
| 8 | `error: winebuild : No such file or directory`（**看起来**像缺 winebuild） | 这行是 `tools/winebuild/utils.c` 里 `fatal_perror("winebuild")` 的**固定文案**；真实原因是 winebuild 要裸名 spawn `clang`（`tools/tools.h:find_clang_tool` 用它找 ar/ranlib），而**我们脚本的 `export PATH="$LLVM_MINGW/bin:$PATH"` 只写在"需要重新 configure"的分支里** → 重跑时跳过 configure，PATH 里没有 llvm-mingw | 源码补丁：PATH 导出提到 `build_ohos_unix()` 开头，与是否重配无关 |
| 9 | `dlls/winedmo/libavcodec/pcm_byte_order_reverse_bsf.c`：`AVBSFInternal` / `AVBitStreamFilter.filter\|init` 不匹配 | Valve 11.0 的 winedmo 仍带 media-converter 那套；上游 11.10 已把该文件与 `unix_demuxer_mediaconv.c`（连同 `unix_demuxer.c` 里的调用）整块删除 | **M1 暂排除 winedmo**（configure.ac 注释），W1 后续决定：移植上游删除 or 迁 fork 的 11.10 winedmo |
| 10 | `dlls/winegstreamer/unixlib.c: 'gst/gl/gl.h' file not found` | Valve 11.0 的 winegstreamer 引 `gst/gl/gl.h`（gst-plugins-base 的 GL 头），sysroot-ext 无该子目录；我们 11.10 的对应文件只 include `gst/gst.h`/`video`/`audio`/`tag` | **未解决**：(a) 移植上游这组改动 /(b) 给 sysroot-ext 补 gst/gl 头 /(c) M1 暂排除 winegstreamer（丢媒体能力，倾向不做） |

**M1 现状**：configure 通过、make 编到 `dlls/winegstreamer`（接近收尾）。
剩下的都是**模块级**差异（11.0 与 11.10 之间被上游改过的模块），不再是工具链或流程问题。

本阶段源码补丁留档（只含**有意**改动）：`patches/w1-stage1-valve-tree.diff`
（`configure.ac` + `dlls/ntdll/unix/loader.c`；自动生成的产物不计入）。

### 10.1 M1 收尾的两条路

1. **补依赖**：给 sysroot-ext 补 `gst/gl` 头（来自 gst-plugins-base；我们已经有 `gst_base_build`），
   并把 winedmo 按上游 11.10 的方式改造（删 mediaconv）。→ 保留全部上游能力，但工作量更大。
2. **按 Gate 需要裁剪**：M1 只需要 wineboot/cmd，可先排除这两块把构建跑完，
   把它们的依赖补齐留到 M3（音频/媒体）之前。→ 更快拿到"完整构建"这个里程碑。

## 11. **M1 达成：Proton-Wine-OHOS 候选构建成功**（2026-09-12 15:48）

```text
[BUILD]   wineserver → libwineserver.so (aarch64-linux-ohos)
[BUILD]   → /data/src/winehua/entry/libs/arm64-v8a/libwineserver.so
[BUILD] Wine 构建完成
```

**零错误**跑完整条链：autoconf →（上游 autogen.sh 等价的）生成器 → configure →
make（147522 行日志）→ wineserver。产物清点（`build/wine-ohos-aarch64/`）：

```text
unix .so     187 个
PE .dll     5184 个
.exe         599 个
libwineserver.so  entry/libs/arm64-v8a/libwineserver.so (1.07 MB)
```

即：**ValveSoftware/wine@dc26e618（Proton 11 Wine Core）已经被我们的 OHOS 工具链编成套**，
过程中使用的是我们自己的 `scripts/build_wine.sh`，Wine 源码侧只加了本文 §9/§10 记录的
那几处 OHOS 补丁（`patches/w1-stage1-valve-tree.diff` 及其后续）。

### 11.1 期间修掉的最后一个坑（也是我自己造成的）

`sysroot-ext` 里的 `.pc` 记录的是**构建当时的挂载点** `/workspace`，而我们现在挂在
`/data/src/winehua`。第一版修复脚本用 `grep -rl` 扫了整棵树，把 **ELF 二进制里内嵌的
路径字符串也改了** —— `/workspace`(10) → `/data/src/winehua`(18) 长度不同，直接把
`libgstreamer-1.0.so` 等库**改坏**，症状是链接报 `gst_debug_log` 之类“未定义符号”。

处置：重新从产品工作树拷贝 `sysroot-ext`，并把脚本收紧到只改 `*.pc` / `*.cmake`。
**教训（写进脚本注释）**：批量 `sed` 必须限定扩展名，绝不能对整棵树做替换。

### 11.2 M1 之后

| 步 | 内容 | 状态 |
| --- | --- | --- |
| M1 | 构建成套（本文件 §11） | ✅ 已完成 |
| M2 | 打包进 HAP / 部署到设备，跑 Gate W0（wineboot / cmd / reg） | 待做 |
| M3 | Gate W1–W5（32/64 WoW64、FEX、GUI、TLS、IPC），再进 S0（Steam） | 待做 |

**注意**：M1 只证明「编得出来」，**不等于**「跑得起来」。
下一步必须把产物做成运行时包部署到 MLR-AL10，用 Gate W0 验证 prefix 建立与进程启动。

## 8. W1 动手前实测到的三个环境障碍（2026-09-12 实做）

### 8.1 子模块 gitdir 是共享的 —— 不要在新工作树里跑 `submodule update`

`git worktree` 出来的新工作树与产品工作树**共用同一个子模块 git 目录**：

```text
唯一存在: /home/liufeng/src/WineHua-arm64ec/.git/modules/thirdparty/wine
git 版本: 2.34.1（不支持 per-worktree submodule）
```

在新工作树里执行 `git submodule update --init` 会复用/争用这一个 git 目录，
**存在改到产品工作树子模块 HEAD 的风险**。

本轮的处置（已验证安全）：

```bash
cd /home/liufeng/src/WineHua-proton-ohos
git clone /home/liufeng/src/WineHua-arm64ec/thirdparty/wine thirdparty/wine   # 硬链接对象，1.4 s
cd thirdparty/wine
git remote add valve https://github.com/ValveSoftware/wine.git
git fetch --depth 1 --filter=blob:none --no-tags valve dc26e61847081a1b5cb0733dc30feba6ee575482
```

得到一份**完全独立**的 Wine 仓库（`origin` 仍指向本地 winehua 克隆，另加 `valve` 参考 remote），
产品工作树与 parity 工作树**均未被触碰**。

### 8.2 `scripts/env.sh` 把 WINE_SRC / BUILD_DIR 写死

```sh
WINE_SRC="$ROOT/thirdparty/wine"
BUILD_DIR="$ROOT/build"
```

是普通赋值（不是 `${VAR:-default}`），**无法用环境变量覆盖**。W1 必须先解决：

- 推荐：在**新工作树**里把 env.sh 改成 `${WINE_SRC:-$ROOT/thirdparty/wine}` 形式；
- 或者写一个 wrapper：先 `source scripts/env.sh`，再覆盖这两个变量（仅当 build 脚本在其后读取它们）。

### 8.3 新工作树没有任何构建依赖树

`<arm64ec>/build` 有 **8.4 GB**（OHOS `sysroot-ext`、wayland/xkbcommon/freetype/gnutls、
各 `*-build` 中间产物）；新工作树里是空的。三条路：

| 方案 | 代价 | 风险 |
| --- | --- | --- |
| 复制整个 build/ 并批量改绝对路径 | 磁盘够（750 G 可用） | 交叉文件里的 sysroot 路径要逐个 sed，容易漏 |
| 在新工作树重跑 `build_deps.sh` | 干净但耗时（小时级） | 无 |
| 借 arm64ec 的 build/ 运行，只把 WINE_SRC 指到新工作树 | 最省事 | **不推荐**：会覆盖产品工作树的构建产物 |

**建议的 M1 路线**：先做**最小化构建**——只出 `ntdll.so` / `wineserver` / 少量 PE DLL，
configure 时 `--without-wayland --without-x --without-alsa --without-opengl --without-vulkan`
（与我们 `build_wine.sh` 里 native-tools 那一步同款做法），把工具链路径先打通；
等 W2 的 Gate 通过，再决定是否复制完整依赖树。

## 9. W1 实做记录（2026-09-12，M1 第一次尝试）

做法：在新工作树里挂 `winehua-dev` 容器（项目挂 `/data/src/winehua`、SDK 挂 `/apps/harmony`、
llvm-mingw 挂 `/data/llvm`），用 `scripts/w1-m1-probe.sh` 让**我们自己的 `build_wine.sh`**
去编 **Valve 的源码树**（`thirdparty/wine-valve` = `dc26e618` 的独立检出），
`NATIVE_ARCH=arm64-v8a`。日志：`build/w1-m1-build.log`。

### 9.1 结果：**构建系统风险被实测证实**，连续撞上同一类问题

| # | 现象（原文） | 根因 | 处置 |
| --- | --- | --- | --- |
| 1 | `config.status: error: cannot find input file: 'include/config.h.in'` | `include/config.h.in` 是 autoheader 生成物；**我们的 fork 把它提交进了仓库，Valve 的 11.0 没有**（WineHQ 11.10 也有） | 已在 `build_wine.sh` 补：源码树缺它时在树内跑 `autoheader`（容器里的 autoheader 是精简版，不支持 `-o/--output`，只能按默认落点） |
| 2 | `error: open wine/vulkan.h : No such file or directory` → `config.status: error: could not create Makefile` | `include/wine/vulkan.h`（+ `dlls/winevulkan/{loader,vulkan}_thunks.*`）同为生成物，**fork 提交了、11.0 没有**（WineHQ 11.10 有） | 已在 `build_wine.sh` 补：缺它时用源树自带的 `dlls/winevulkan/make_vulkan -x vk.xml -X video.xml` 生成（实测生成到**源码树**，不是构建目录） |
| 3 | `dlls/ntdll/signal_arm.c:35: error: ntsyscalls.h: No such file or directory` → `config.status: error: could not create Makefile` | 同一类：`dlls/ntdll/ntsyscalls.h` 由 `tools/make_specfiles` 生成；**fork 提交了、11.0 没有**（WineHQ 11.10 有）。且它**版本敏感**（是系统调用号表），不能从 11.10 抄 | **未解决**：见 §9.2 |

**结论 1**：这不是偶发问题，而是一整类——`proton_11.0` 分支缺少若干"上游提交进仓库的生成物"，
而我们的构建流程默认它们在源码树里。

**结论 2**：**不能把 11.10 的生成物直接抄进 11.0 树**。`config.h.in` / `vulkan.h` 抄了问题不大，
但 `ntsyscalls.h` 是 syscall 号表，抄错会让 ntdll 静默错乱 —— 必须用该树自己的生成器产出。

**结论 3**：这正好印证了 §1 里"构建系统风险最高"的判断，也说明 W-19 必须最先做、且要按
**"生成物"**这条线单独过一遍，而不是只看 `Makefile.in` 有没有对应的模块。

### 9.2 下一步（二选一，倾向 B）

**A. 继续把 out-of-tree 流程补齐**：在 `build_native_tools` 之前先把 host 工具
（`tools/make_specfiles` 等）编出来，并按依赖顺序生成 `ntsyscalls.h` / `server_protocol.h` 等。
工作量大、依赖顺序易错（Wine 的生成器之间互相依赖）。

**B. 改用上游自己的流程（推荐先试）**：在 Valve 源码树里直接 `./autogen.sh && ./configure && make`
（in-tree），让 Wine 自己的规则把生成物写进源码树；等 M1 跑通、Gate 通过之后，
再把 in-tree 产物接入我们现有的打包流程（`assemble.sh` 取 `$BUILD_DIR` 里的产物）。
理由：我们踩到的三个坑全部是"生成物不在源码树里"导致的 in-tree/out-of-tree 差异，
而 Wine 官方流程从来不支持 out-of-tree 到这种程度。

### 9.3 本轮同时完成的 W1 准备工作

- `scripts/env.sh`：`WINE_SRC` / `BUILD_DIR` 改为可被环境变量覆盖（默认值不变）。
- `scripts/w1-m1-probe.sh`：新增，`env|autoconf|full` 三个子命令，可复现本轮全部动作。
- `thirdparty/wine-valve`：Valve `dc26e618` 的独立检出（`origin` 仍指 winehua 本地克隆，
  另有 `valve` remote）；`thirdparty/wine` 与产品工作树**未被触碰**。
