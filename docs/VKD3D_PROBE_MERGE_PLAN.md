# VKD3D Probe 合并计划

> `feature/vkd3d-probe-merge-master` → `master`
> 更新: 2026-08-19
> 状态: 📋 已 review 完成，待合并

---

## 一、概述

`feature/vkd3d-probe-merge-master` 是 VKD3D capability probe 分支合并 master 的结果，目标是把 VKD3D 2.6（500K descriptor 实验）能力 + DXVK modern 双 runtime 纳入 master 主链路。

**分支状态**：feature 分支完全包含 master（`feature..master` 为空），可直接 merge，无需冲突解决。submodule 6 处变化全部已 push remote。

**改动规模**：主仓库 85 文件 +12975/-366；submodule 6 个（4 更新 + 2 新增）；18 个 vkd3d-proton patch（全部 cleanly apply，hash-verified 声明成立）。

相关文档：[VKD3D_LIMITED_500K_PLAN.md](VKD3D_LIMITED_500K_PLAN.md)、[VKD3D_DESCRIPTOR_500K_910_20260804.md](VKD3D_DESCRIPTOR_500K_910_20260804.md)、[DXVK_MODERN_UPGRADE_READINESS.md](DXVK_MODERN_UPGRADE_READINESS.md)

---

## 二、合并步骤

### 阶段 0：合并前必做（阻断项）

| # | 事项 | 说明 |
|---|------|------|
| B1 | 验证 HAP 构建确实 stage `vkd3d/limited-500k` + `dxvk/modern-2.6` | 本地 staging 现状只有 `dxvk/legacy`，无 vkd3d/ 与 dxvk/modern-2.6；默认 backend 已翻转为 vkd3d_limited_500k，缺 overlay 会静默无 D3D12（见风险 R1） |
| B2 | 确认默认 D3D backend 翻转是产品决策 | 新鲜安装默认从 `dxvk_legacy` → `vkd3d_limited_500k`，D3D12 游戏默认行为变化（见风险 R1） |
| B3 | C++ wineboot 状态文件改动与 wine submodule 指针同批合并 | `wine_launch.cpp` 等待 "wineboot-init-ok"，只由 wine 78dffe3e 写入；不同批会首启卡 60s（见风险 R2） |
| B4 | vkd3d-proton / dxvk-modern 补上 `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC` 消费端 | 目前无消费代码，venus persistent-map-sync 对 D3D12 上传堆无效（见风险 R4） |
| B5 | wine 树 vendor `smoke/vkd3d_capability_audit.h` | wine 编译逃逸到主仓库，破坏 wine 独立可构建（见风险 R3） |

### 阶段 1：submodule 合入 default

对每个变化的 submodule，按 [SUBMODULE_MAINTAINABILITY.md](SUBMODULE_MAINTAINABILITY.md) 流程：

**需先合入 default 的 4 个**（commit 只在 feature 分支）：

| submodule | HEAD | default 分支 | 操作 |
|-----------|------|-------------|------|
| wine | 78dffe3e | master | merge `feature/vkd3d-capability-probe` → master，更新指针 |
| mesa | 2939cbb8 | main | 同上 |
| virglrenderer | e643158f | master | 同上 |
| dxvk-modern | 977a3d78 | dxvk-modern-2.6 | 同上 |

**已在 default 历史的 2 个**（可选更新指针到 default 尖端）：

| submodule | HEAD | 说明 |
|-----------|------|------|
| dxvk | 5058927a | 在 dxvk-legacy-1.10.3 历史，落后尖端 1 commit |
| vkd3d-proton | 3e5aab6f | 在 master 历史，落后尖端 |

### 阶段 2：主仓库合并

```bash
git checkout master
git pull
git merge feature/vkd3d-probe-merge-master --ff-only   # 或普通 merge
git push origin master
```

> 已验证：`feature..master` 为空，wine 用 `git merge-tree` 验证合并保留 master 版本、零冲突。diff 中的大段删除（WINEHUA_SIMULATE_RESOLUTION 等）是 fork 点落后假象，非真实改动。

### 阶段 3：合并后验证

1. **本地构建**：`make NATIVE_ARCH=arm64-v8a`（验证 staging 产物：vkd3d/limited-500k + dxvk/modern-2.6 + dxvk/legacy）
2. **设备回归**：三游戏 + 桌面会话 + 多 prefix
3. **D3D12 专项**：在 "full" from-host 模式下跑多帧在飞的 DXVK 2.x 游戏（红警2/PAL2 之外选一个），验证无贴图/常量错乱（见风险 R5）
4. **clean prefix 首启**：验证 wineboot 状态文件握手正常
5. **CI**：verify embedded runtime 步骤（dxvk manifest schema 1/2 + sha256 对账 + vkd3d upstreamCommit）

---

## 三、风险清单（review 发现）

### 🔴 合并阻断

| ID | 风险 | 位置 | 建议 |
|----|------|------|------|
| R1 | 默认 D3D backend 翻转 vkd3d_limited_500k，staging 无对应 overlay，默认路径静默无 D3D12 | `wine_env.cpp:157-266`、`EntryAbility.ets:48,280`、`Index.ets:39-40,74`、`wine_exe.h:16` | 阶段 0 B1/B2 确认 |
| R2 | wineboot 完成判定硬依赖 submodule 状态文件，C++ 与 wine 指针不同批合并则首启卡 60s | `wine_launch.cpp:510-529,598-608` + `wine/dlls/ntdll/unix/env.c:1539-1567` | 阶段 0 B3 |
| R3 | wine 编译逃逸依赖主仓库头文件，wine 无法独立构建，破坏「master 永远可构建」 | `wine/programs/winehua_vulkan_smoke/main.c:18` | 阶段 0 B5 vendor 头文件 |
| R4 | venus persistent-map-sync 对 D3D12 无效：主仓库设 `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1` 但 vkd3d-proton/dxvk-modern 无消费端 | `mesa/vn_device_memory.c:649-654` | 阶段 0 B4 |

### 🟠 高风险运行时

| ID | 风险 | 位置 |
|----|------|------|
| R5 | timeline semaphore 触发 from-host 批量同步竞态：sem 查询成功≠GPU 完成，多帧在飞时覆盖未提交 guest 写 → 数据损坏（fence 路径有保护，semaphore 路径没有） | `virglrenderer/vkr_device_memory.c:2250-2274`、`vkr_queue.c:1695,1710` |
| R6 | SIGCHLD handler 内 async-signal-unsafe I/O（fopen/fgets/sscanf），打断 malloc/stdio 会死锁，每次 SIGKILL 都触发 | `wine_process.cpp:455-481`（`ReadWineServerExitTelemetry` 423-452） |
| R7 | in-process VirGL Stop 阻塞 UI 线程 ≤5s，超时残留 running 状态，stale Venus ring 泄漏进下个会话 | `graphics_broker.cpp:776-831` |
| R8 | loader 托管 overlay 强制覆盖应用私有 DLL（d3d12→VKD3D_ROOT，d3d11/dxgi→DXVK_ROOT），DRM/反作弊自带 d3d12.dll 的游戏被遮蔽 | `wine/ntdll/loader.c:3313-3332` |
| R9 | host_vulkan_probe：无 timelineSemaphore 设备被提前 abort（跳过 swapchain/command probe）；Vulkan 1.2 入口未按 loader 版本 guard（1.1 loader 上 NULL 崩溃） | `host_vulkan_probe.cpp:307,474,570-633` |

### 🟡 中等

| ID | 风险 | 位置 |
|----|------|------|
| R10 | Wine Mono 下载失败 warn→err 硬失败，离线/弱网 `make deps` 直接挂（CI 靠 BUILD_WINE_MONO=0 规避） | `build_deps.sh:84-105` |
| R11 | `DXVK_MODERN_SOURCE_INPUTS` 漏 include/**，bump 指针只改 include 时 stamp 不失效 → 静默打包旧 DLL | `Makefile:48` |
| R12 | 0014 patch profiled Device10 wrapper 丢参数（CreatePlacedResource2 缺 heap）+ FIXME 格式串参数不匹配 UB（profiling 开启时触发） | `patches/vkd3d-proton/0014` |
| R13 | run_regression.py Docker 构建产物与 WSL HAP 路径不同步（静默测陈旧代码）；Linux/Windows hdc 混用 + /data 路径不转换 | `automation/run_regression.py:269-296,137-172,1104` |
| R14 | SmokeRunner `stageManagedVkd3dRuntime` 死代码，注释与唯一实际 D3D12 smoke 路径矛盾；Index 手动 smoke 超时不终止进程（可双开）+ title filter 页面销毁泄漏 | `SmokeRunner.ets:287`、`Index.ets:487,502-556` |
| R15 | dxvk divisor 模拟读陈旧 CPU 映射（UpdateSubresource/GPU 写路径）；每 draw 全量 SHA1 + 全量 memcpy（性能） | `dxvk/d3d11_context.cpp:1319-1330` |
| R16 | mesa venus 每次 submit 对写过映射全量同步 flush + 持锁 + 同步往返（性能/锁粒度） | `mesa/vn_device_memory.c:73-121` |
| R17 | smoke Vulkan 1.1 + descriptor_indexing 设备误报 UNSUPPORTED（arm64-native 路径；dxvk26_requirements 已正确处理，guest_vulkan_smoke 没同步） | `smoke/guest_vulkan_smoke.c:482-550` |
| R18 | smoke 在 1.1 instance 上直接调 Vulkan 1.3 命令（vkQueueSubmit2 等），loader 可能返回 NULL | `smoke/winehua_dxvk26_requirements.c:1325-1337` |

### ⚪ Minor（不阻塞合并，记录）

- **dxvk** present sRGB 编码未按 env 门控（x86_64 桌面行为变更，建议回归）；A2C spec constant ID 中间态已由 5058927a 修复（cherry-pick 中间 commit 是坏的，须拿最终态）；A2C alpha 阈值与 D3D 阈值不一致（已声明取舍）
- **mesa** `VN_WINEHUA_ALWAYS_NOTIFY_RING` 未在主仓库启用，该修复处于休眠态；vn_Flush 同步往返在持锁下阻塞
- **virglrenderer** invalidate 冲刷后 host_map 可能 NULL（默认 env 关）；sRGB 改动影响 GLX 桌面路径；diagnostic UBO binding 0/3 共享槽位；descriptor_set_layout destroy 例外不完整
- **wine** vulkan.c TRANSFER_DST 无条件加在私有 swapchain 镜像；wineboot 状态通道 write 返回值未检查；audit JSON 拼接可能截断；d3d11_smoke 误标 mixed 会话
- **C++** `TerminateWineProcess` 日志 stale errno；`ForceSourceClearEnabled` 每帧 getenv；exit-telemetry 文件无限增长 + pid 复用误读；`gBrokerHomeDir`/`gBrokerPrefixDir` 无锁全局；winehua_keep.exe 缺失变成硬失败
- **ArkTS/CI** WineWindowManager title filter created/title 竞态；`vkd3dSmokePhase==='pass'` 颜色分支不可达；REQUIRED_PAYLOAD 漏 vkd3d 产物；`--suite vkd3d-capability` 跳过 host-vulkan 视觉校验；VN_PERF 补丁缩进误导；vkd3d 环境变量白名单不完整
- **smoke** dxvk26-requirements 空 command buffer（fragile）；d3d_switch_cube 时钟 gating；write_result 硬编码 x86_64 架构（arm64-native 误标）；vkd3d_limited_write_checkpoint 字面 `\n`；0008 blit 结果未检查；0014 声称 Device10 但 Enhanced Barriers 会失败（须 manifest 注明）

---

## 四、待改进项（合并后跟踪）

按优先级：

### P1（影响正确性）

- [ ] R5：virglrenderer from-host 批量同步加「dirty 内存先冲刷再读回」守卫（复用 invalidate 的 pending 处理）
- [ ] R6：SIGCHLD handler 只记录 pid，文件 I/O 挪到 reaper 线程
- [ ] R7：in-process VirGL Stop 超时标记 zombie，下个会话强制重启
- [ ] R17/R18：smoke 层对齐 descriptor_indexing 查询 + 1.3 命令走 vkGetDeviceProcAddr
- [ ] R12：0014 patch profiled wrapper 补全参数 + FIXME 格式串修正

### P2（影响性能/构建）

- [ ] R15：dxvk divisor 用 generation/脏标记替代每 draw 全量 SHA1
- [ ] R16：mesa venus 合并 flush 范围 + 用 async flush 免往返
- [ ] R11：Makefile DXVK_MODERN_SOURCE_INPUTS 补 include/**
- [ ] R10：build_deps Wine Mono 失败降级 warn（或文档注明强依赖外网）

### P3（流程/工具链）

- [ ] R13：run_regression.py 加 bind-mount 检查 + 统一 hdc 调用方式 + 路径转换
- [ ] R14：删除 SmokeRunner 死代码 + Index 手动 smoke 超时终止进程 + title filter 页面生命周期清理
- [ ] R3：wine 树 vendor smoke 头文件（合并前必做，见 B5）
- [ ] B4：vkd3d-proton/dxvk-modern 补 `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC` 消费端（合并前必做）

---

## 五、结论

**合并可行性**：改动经 7 领域 review，18 patch cleanly apply，无 CRITICAL 阻断性代码缺陷；主要风险集中在「默认 backend 翻转 + overlay 未 stage」的构建产物配套（R1/R2/R4 均属合并协调问题，非代码 bug）。

**合并前置条件**：完成阶段 0 的 B1-B5 后即可执行阶段 1-3。风险 R5（from-host 同步竞态）建议在阶段 3 用多帧在飞游戏实测，若复现则阻止发布。
