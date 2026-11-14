# HGS 双协议迁移 P0 基线账本

日期：2026-11-06。状态：**P0 基线采集与归档完成，存在明确失败项。两套源码/依赖已冻结，主机测试、native/HAP 构建、本地签名验签、同设备 Wayland/X11 图形与窗口/输入回归均已留档。X11 图形结果使用独立无 Mono 配置；完整配置首启、帧归还同步及两路线部分功能仍失败，不能视为产品全绿。**

本阶段不修改图形栈实现。依据 [迁移规划](../hgs-dual-protocol-service-plan.md) 的 P0，建立后续变更可对照的来源、依赖、冲突和测试记录。

## 冻结版本与工作树

| 对象 | 版本 / 位置 |
|---|---|
| WineHua 分支基线 | `3355dab96b968cd7e6d3da1e01e1a9c7c0b831fc` |
| HGS 实际基线 | `bad402da1a8bdb56c87c2010a2727c09fc58251b` |
| 上游 X11 基线 | `a001e6d0ca277d5a5fb2b4bd6a030ac52e5e132e` |
| merge base | `3eecbd732d79bf6d306fdb48eee734771eb58147` |
| BetterSH 基线 | `7392c3dba391151b69810d80d03b805af05ab879`，实际 HGS 同为 bad402d |
| 集成工作树 | `/workspace/WineHua-root/hgs-integration` |
| 集成 HGS 工作树 | `/workspace/WineHua-root/hgs-integration/hgs` |
| 两仓库同名功能分支 | `feature/hgs-dual-protocol` |
| 上游隔离对照工作树 | `/workspace/WineHua-root/hgs-upstream-baseline`，detached HEAD |

原工作区保持在 `feature/hgs-har-scaffold`。集成树的外层 gitlink 在 HEAD 中仍是 bad4d5f，HGS 工作树为 bad402d，因此外层 `M hgs` 是已记录的基线差异，不是迁移实现。尚未提交/推送子模块指针。

集成工作树内的 HGS 是通过 `git worktree` 创建的共享对象库工作树；P0 阶段不要在这里执行 `git submodule update hgs` 覆盖它。其他第三方子模块未批量初始化。上游测试起初只补齐两个 Wine 验证输入；后续构建已准备好锁定提交的完整 Wine 工作树，以及 X11 host/guest 依赖源码。

`baseline-lock.json` 固定完整 SHA、两份 clone 的远程跟踪状态、所有 gitlink、初始工作区状态及公共头/.d.ts 哈希。它是 P0 证据锁，尚未接入 HGS 构建系统。

WineHua 的 HGS clone 本地 `origin/main` 是 bad4d5f，领先计数为 **11**；BetterSH 的 HGS clone 本地 `origin/main` 是 b316deb，领先计数为 **7**。这是两份 clone 的跟踪引用不同；本次未联网刷新或核实远程发布情况。不能将任一领先计数直接解释为实际未推送数量。

## 保存已有修改

备份位置：`/workspace/WineHua-root/WineHua/build/hgs-p0/2026-11-06/`。

- `snapshot/<repo>/index.patch`：暂存区二进制补丁。
- `snapshot/<repo>/worktree.patch`：未暂存二进制补丁。
- `snapshot/<repo>/head.patch`：相对 HEAD 的完整二进制补丁。
- `snapshot/<repo>/untracked.tar.gz`：Git 未忽略的未跟踪文件，包含原交接文档和迁移规划。
- `hgs-baseline.bundle`：HGS bad402d 的完整提交历史，已通过 `git bundle verify`。
- `SHA256SUMS`：本阶段原始证据的校验清单。

覆盖 WineHua、HGS、glib、gstreamer、pcre2、dxvk-modern、BetterSH、BetterSH 内 HGS。备份保留了 BetterSH 已有 EntryAbility 修改。未 stash、reset 或改写原分支；也未备份被 Git 忽略的整套 build/SDK 缓存，避免把可再生产物当成源码状态。

恢复时先核对原始 HEAD，在单独工作树应用补丁并解包未跟踪文件；不要直接对正在工作的目录覆盖。bundle 可用于从本地重新取回 HGS 基线提交。

## 上游变更与迁移清单

本次 `git merge-tree --write-tree --name-only HEAD upstream/master` 返回 1，表示存在预期合并冲突。它没有执行真实 merge，也没有修改任何工作树或 index。

- 上游相对 merge base：**313 个提交**，详见 `upstream-commits.tsv`。
- 冲突：**42 个**，其中 **28 个 modify/delete、14 个 content**，详见 `merge-conflicts.tsv`。
- X11 display 栈：**22 个文件**，详见 `migration-inventory.tsv`，逐文件记录目标域与原始内容 SHA256。
- 原始合并输出、上游全部文件变更清单保存在 build 证据目录。

modify/delete 文件继续保持 entry 删除状态，将上游 delta 移植进 HGS 对应文件。X11 新栈迁入独立 backend；窗口/输入调用点改为按窗口所属 backend 分发。具体逐文件目标在 TSV 中，不提前实施。

## 依赖基线

`dependencies.json` 记录上游全部子模块 gitlink、7 个关键构建脚本哈希、8 个 OHOS 补丁哈希、wlroots 补丁应用次序、15 个源码归档 URL/hash，以及另外 2 个通过脚本变量配置的 host 归档。

已锁定的主要版本：wlroots 0.20.2、Xwayland 24.1.13、Wayland 1.26.0、host pixman 0.46.4、host libdrm 2.4.134。xkbcommon 等按 upstream 精确 gitlink 固定。

当前 HGS/Wayland 分支仍使用原依赖，尚未升级。后续 P1/P2 应区分“旧基线依赖”和“目标后端依赖”，处理 host arm64 / guest x86_64、相同 SONAME 的唯一打包来源。X11 基线构建已在隔离树运行依赖脚本并应用所记录的上游补丁；原工作区第三方源码未改动。

## 本次测试

| 项目 | 结果 | 范围 |
|---|---|---|
| HGS/Wayland 集成树 `make test` | **PASS，exit 0** | 16 个测试程序；包括 HGS endpoint/event/window manager/native keymap、几何、输入和 controller |
| 上游树 `make test` | **PASS，exit 0** | 12 个测试程序，以及 WHGP v2 host/Wine 一致性验证 |
| merge-tree | **预期冲突，exit 1** | 42 个冲突，与交接数量一致 |
| HGS bundle verify | **PASS** | bad402d 完整历史可读取 |
| HGS ARM64 native 新构建 | **PASS** | 干净目录编译 62 步，生成 libhgs.so 和 libsmoke_napi.so |
| WineHua Wayland 未签名 HAP | **PASS** | Hvigor 43 个任务全部执行；entry/HGS 原生层和 ArkTS 均重新编译 |
| 上游 X11 host 原生构建 | **PASS** | 锁定的 wlroots/Xwayland、host 依赖链和 94 步 entry/native child 编译完成 |
| 上游 X11 guest / Mesa | **PASS** | 新 Wine x64/x86、winex11、OpenGL/VirGL、Venus 均已编译，158 个测试程序新编译，engine zip 组装完成 |
| 上游 X11 HAP / 签名 / 安装 | **PASS** | native / ArkTS 打包、本地完整验签和同设备安装成功 |
| 签名工具 doctor / selftest | **PASS** | Java、keytool、签名工具可用；本地回调和临时密钥生成通过 |
| 真机安装/验签 | **PASS** | 使用用户指定材料，本地验签及设备安装成功 |
| Wayland core | **PASS，4/4** | OpenGL x64/x86 结果与截图均通过；DNS x64/x86 通过 |
| Wayland DXVK | **PASS，3/3** | legacy x64/x86 与立方体出图通过 |
| Wayland wine-vulkan | **FAIL，3/4** | offscreen x64 shader 采样值不匹配；按实际失败留档 |
| Wayland 窗口/输入子集 | **FAIL，7/8** | 鼠标、键盘、点击、缩放、滚轮、菜单、属主窗通过；Z 序 send-a-bottom / bring-b-top 失败 |

详细命令、测试程序列表、日志路径和哈希在 `baseline-tests.json`。

上游首次测试因新工作树未初始化 Wine 子模块而缺少验证输入。补齐 `025656dfd3920ea11aaab9e8ac6d905ec35963fe` 的 `dlls/winebus.sys/winehua_gamepad_protocol.h` 和 `bus_ohos.c` 后，完整重跑通过；没有修改测试或被测源码。

主机测试复现：

```bash
make -C /workspace/WineHua-root/hgs-integration test
make -C /workspace/WineHua-root/hgs-upstream-baseline test
```

上游测试树重建时，可以初始化锁定 Wine 子模块；若只复现当前纯主机验证，也可从原 Wine clone 用 `git show <锁定提交>:<上述文件路径>` 导出这两个文件。导出目标必须在隔离工作树内，不能覆盖原 Wine 源码。

## 真机条件与历史问题账本

HDC 在沙箱内连接 server 失败；使用沙箱外只读检查发现在线设备 `192.168.3.9:46071`。包查询与成功的应用列表核查表明，初次核查时该设备未安装 `app.hackeris.winehua`；本阶段随后已安装并完成下述测试。这里只记录任务相关结论，不保存设备完整应用列表。

本次已在隔离集成树生成新的 unsigned HAP：`/workspace/WineHua-root/hgs-integration/entry/build/default/outputs/default/entry-default-unsigned.hap`，398,098,027 字节，SHA256 `398cb72be3a16103b8a1e1d1a1f35d8b6aaefe6e49698f86e66f50de91784428`。应用原生层和 ArkTS 为本次编译；Wine 引擎 zip 与第三方运行库复用原工作区产物，**没有重编 guest 引擎，也未验证其完整源码来源**。38 个复用文件的哈希与 HAP 内主要 ARM64 原生库的哈希记录于 `build-artifacts.json`，不能把此包描述为全链从源码重新构建。

原 HGS CMake cache 指向旧路径，本次独立 native 构建使用全新证据目录，不复用旧对象文件。`build-baseline.mk` 保存实际构建入口，可与根 Makefile 一起使用：

```bash
cd /workspace/WineHua-root/hgs-integration
make -f Makefile -f /workspace/WineHua-root/WineHua/docs/architecture/hgs-p0/build-baseline.mk ROOT="$PWD" p0-hgs-native
make -f Makefile -f /workspace/WineHua-root/WineHua/docs/architecture/hgs-p0/build-baseline.mk ROOT="$PWD" p0-hap-unsigned
```

未签名 HAP 的准备步骤包括：复制并记录既有 ARM64 runtime libs/rawfile；复用 oh_modules 的依赖包和相对本地链接；仅在隔离树去掉签名配置并设置 ARM64 ABI。原工作区签名配置未修改。Hvigor 需要写用户缓存，已通过沙箱外构建完成。

已阅读 `/workspace/signer/hapsign_auto.py` 与 `devsign_material.py`。前者负责浏览器登录、云端调试证书/设备/Profile 申请和本地 HAP 签名；后者只生成 Hvigor 密码加密材料，不能替代有效的签名证书/Profile。工具链检查与自检均已通过。用户随后指定 `/workspace/signer/signing_files/app.hackeris.winehua`；按此指令停止了在线申请，使用该目录的证书、Profile、密钥库和密码解密材料完成本地签名。Profile 匹配 WineHua 包名及设备，有效期至 2027-07-19。签名包通过 digest/permission 完整验证并安装成功。签名工具调用未打印密码；私钥和令牌不进入本账本或版本库。签名包哈希见 `build-artifacts.json`。

本次设备测试已分别固定路线、backend、virtual 桌面模式，并按 case 归档结果。Wayland 使用既有 guest 引擎，X11 使用新 Wine/Mesa 与无 Mono 图形配置，两者是独立现状基线，不是只改变显示路线的严格 A/B 实验。每轮通过强制停止、重新启动编排覆盖冷启；尚未覆盖长期重复启停压力、完整多窗口融合承载矩阵或 Wayland/X11 同开（后者尚未实现）。这些不作为 P0 已完成的测试范围。

以下是上游 `a001e6d0c:docs/engineering/display-route-known-issues.md` 的历史记录，**本次没有重新验证，也不直接豁免后续失败**：

| 历史事项 | 后续处理 |
|---|---|
| X11 GL 启动期 `rel_unsynced_presented=3`，记录为尚未查清 | 记录每次运行计数和帧阶段；不能把它无条件改判 PASS |
| 最后一键间歇未进入控件、pending key 单槽 | 单独记录输入场景与丢键情况 |
| XIM static spawned 提前置位、会话重启存活行为未定 | 覆盖失败重试和 start/stop/start |
| dxvk-legacy 初始化失败与设备能力相关 | 按实际设备记录，不跨设备照搬结论 |
| 曾标记为 opengl-x86 D49 的部分失败，后续被重新判定为测试归属问题 | 不能沿用早期“3/4 即合格”的口径；按最新判定与原始数据重新评估 |

## 采集修复与当前设备结果

主机采集脚本原先把尚未存在的 suite-summary 的 `cat` 错误当成整轮读取失败，丢弃了同一轮已经成功读取的 fixed-frame。只在集成工作树修复复合读取退出状态，并补齐归档重判时的选测过滤；3 个回归测试通过。补丁是 `wayland-smoke-harness.patch`，测试副本是 `smoke_sandbox_texts_test.py`，实际执行位置为集成树 host_tests。

当前有效设备归档位于 build 证据目录的 `device-runs/`：`core-p0-wayland-core-20261106`（4/4）、`dxvk-p0-wayland-dxvk-20261106`（3/3）、`wine-vulkan-p0-wayland-wine-vulkan-20261106`（3/4）。首次缺帧和缺 NumPy 的运行也保留，但不再作为当前结论；修复后采集成功且对既有归档重判，没有放宽视觉判据。

X11 依赖/host 构建使用隔离工具目录 `.hgs-build-tools`，工具版本/源码归档哈希见 `build-tools-lock.json`。宿主 scanner 与 host/guest pkg-config 目录分开；对已验证的 Meson `.p` 目录误选问题及宿主 FreeType 路径增加构建修复，均只作用于上游隔离树，补丁见 `upstream-build-fixes.patch`。这些不是图形栈功能迁移。

## 下一步与 P0 退出门

- [x] 冻结 WineHua、HGS、上游三个版本，记录 BetterSH 对照。
- [x] 保存已有本地修改与 HGS 离线提交历史。
- [x] 创建主仓库与 HGS 同名功能分支、隔离集成树及上游对照树。
- [x] 生成依赖、补丁、上游提交、冲突和迁移清单。
- [x] 两套主机测试通过并保留日志。
- [x] HGS ARM64 新编译、应用原生层/ArkTS 新编译和未签名 HAP 打包，记录复用 runtime 输入哈希。
- [x] 阅读 signer 流程并验证工具链及本地回调/临时密钥生成。
- [x] Wayland 基线的 native/HAP 本地签名、验签、设备安装，以及 X11 host 原生构建。
- [x] 完成锁定 X11 Wine guest、Mesa 及可安装 HAP，保留完整配置和无 Mono 图形配置的来源、产物及哈希。
- [x] 完成同设备上的 Wayland / X11 图形、窗口/输入与中文 IME 子集回归，分别记录功能结果、初始化失败和同步不变量失败。

P0 的退出目标是冻结可复现现状及失败账本，而不是修复所有产品问题；本阶段基线已建立，P1 公共契约开发可以在隔离工作树继续。后续迁移验收不得豁免本账本中的实际失败，也不得把 X11 功能出图通过等同于同步不变量通过。

## X11 打包复现补充

Wine `025656df`、Mesa `e975a707` 已按锁定版本完成新构建。OpenGL/VirGL 与 Venus ICD 必须顺序调用，脚本共用的交叉文件包含模式相关的 system 字段；同时调用会使 OpenGL 构建被 Venus 配置覆盖，本次已顺序重跑并通过。Vulkan Loader/诊断程序/着色器复用原 runtime，相关构建脚本及源文件在两个分支间无差异；ICD 为新 Mesa 编译。

首次 Hvigor 构建的 clean 删除了隔离树根 build/，导致 wlroots 静态库消失。host-ext 已事先保存于证据目录 `x11-host-ext/`；增加仅用于 P0 的 CMake 覆盖参数 `HGS_P0_HOST_EXT_ROOT`，并在隔离 entry build profile 指向该目录后重试。新引擎及其 Wine/Mesa 运行库保留在 rawfile zip；对象目录已被清理，不作为长期证据。新编的 158 个测试程序从同一个 zip 提取到证据目录 `x11-smoke-payload/`，避免重编或混用旧载荷。

上游 ArkTS 首次编译还暴露两项声明不一致：native `RunWineProgram` 已处理缺省的 `presentBackend`，声明却标为必填；native NAPI 已注册 `getLastPtrWindowId`，声明缺失。隔离树仅对齐声明，补丁为 `upstream-types-build-fixes.patch`；未改 native/ArkTS 图形逻辑。签名 X11 HAP SHA256 为 `0162b8f3ab30efaca7ba6dceffb4f6173c2f5f6bea66fca922db2683fd6469ca`，404,517,667 字节，完整验签及设备安装通过。

## X11 首次初始化与单独图形配置

完整 Mono 包的首轮 core 在 5 分钟内没有 suite-summary；这表示测试尚未执行，不能记为 core 的 0/4 计算失败。设备 `wineboot` 标记停在 `wineboot-dispatch`，Mono 安装调用链已经启动，native 持续等待初始化。原始包保存在 build 证据目录 `x11-full-mono-signed.hap`，对应引擎 `x11-full-mono-runtime.zip`；日志与标记一并保留。EGL import 探针返回 `pass:payload=OHNativeWindowBuffer`，present 探针尚无结果，该探针本身不足以证明完整 X11 出图通过。

为隔离 Mono，按原 `assemble.sh` 的 `BUILD_WINE_MONO=0` 规则派生独立图形测试包：去掉两个 PE 架构中的所有 `.cpl` 和 Mono MSI，共 13 个条目，其他条目的 CRC/长度逐项一致。原 build 对象已由 Hvigor 清理，因此直接从已组装 zip 派生，不能称为重新跑了一次完整 assemble。条目清单、来源和新引擎 hash 见证据目录 `x11-no-mono-profile.json`。此包同样完成本地签名、完整验签与安装，SHA256 `f834cc4fff982cbc0c8c84fdefed38ba72a10834f0c50d954c43929818e04fd8`，320,656,164 字节。它的结果单独记录，不替代完整配置的首启失败。

## 最终设备基线

下表保留测试器原始结论，另列独立的同步不变量失败。X11 均为无 Mono 图形配置，完整配置首启失败单独保留。

| 项目 | HGS / Wayland | 上游 X11 |
|---|---|---|
| core / OpenGL + DNS | PASS 4/4 | 功能与视觉 PASS 4/4；同步检查见下方 |
| DXVK | PASS 3/3 | 上游专用立方体门禁 PASS 1/1，未冒充完整矩阵 |
| Wine Vulkan | FAIL 3/4：offscreen x64 storage_write 采样不符 | FAIL 3/4：sampled-only x86 image_fetch 返回 0xdeadbeef |
| Vulkan present x64 | 本次未单独测试 | PASS 1/1 |
| 窗口/键鼠/中文 IME 子集 | FAIL 7/8，Z 序两项断言失败 | PASS 8/8 |
| 完整 Mono 配置首次初始化 | 所用既有引擎已可运行；未等价重测新 Wine | TIMEOUT，5 分钟内未进入测试 |
| X11 帧归还同步不变量 | 无对应 X11 计数，不做判定 | **FAIL**：core PID 57389 最大 1262；DXVK PID 57650 最大 541；Vulkan present PID 61404 最大 150，均要求为 0 |

`frame-release-audit.json`（build 证据目录）按应用 PID 记录日志计数，避免将跨冷启的 hilog 快照累加为一轮。源码 `display_guest_frames.cpp:250` 明确该非零值是不变量破损；现有 core checker 不检查它，因此不能仅引用 4/4 作为完整 X11 通过结论。已采集固定帧图像并核对 OpenGL 四象限实际出图，未放宽判据。

完整失败条目及后续验收要求见 `known-failures.json`。没有提交、推送、合并上游或实施 P1 运行时迁移；原工作区既有源码修改保留。当前设备安装的是无 Mono 的 X11 图形测试包；需要 .NET 的完整包已另行保留。

最终复核时，WineHua / HGS 的 tracked diff 与初始快照逐字节一致。BetterSH 在本阶段期间已推进到 `7135662`（初始冻结为 `7392c3d`），原 EntryAbility 的本地 diff 已不再以同样形式存在，另有 iec 子模块 dirty；这属于此任务之外的工作区变化。本任务未修改/回退 BetterSH，初始锁和快照仍保留原状态，复核记录见 `final-workspace-audit.json`。
