# Wine for HarmonyOS — 技术文档

## 项目概述

将 Wine 移植到 HarmonyOS (OpenHarmony)，使 Windows 程序在鸿蒙系统上运行。

当前架构：Wine (x86_64, musl) + Box64 (ARM64) → Wayland compositor → 鸿蒙 XComponent 上屏。
渲染栈：guest DXVK 1.10.3 (D3D11) → Wine Vulkan → Mesa Venus (vtest) → virglrenderer Venus → host Vulkan；WineD3D/VirGL (OpenGL) 为显式 fallback。

## 文档索引

### 当前状态
- **[CURRENT_STATUS.md](CURRENT_STATUS.md)** — 里程碑、已修复问题、已知问题
- **[PHASE2_DXVK_STATUS_MEMO.md](PHASE2_DXVK_STATUS_MEMO.md)** — DXVK 调查活文档（handoff，改 DXVK/Venus/present 前必读）

### 架构与设计
- **[ARCHITECTURE_OVERVIEW.md](ARCHITECTURE_OVERVIEW.md)** — 总架构图（四域：wine / compositor / 音频 / 图形 + 进程拓扑 + 模块索引），**首次接触项目从这里读**
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — Wine 内部架构、Wayland compositor 设计
- **[OPENGL_VIRGL_DESIGN.md](OPENGL_VIRGL_DESIGN.md)** — VirGL/OpenGL 设计与 zero-copy/Vulkan 演进
- **[CROSS_FORK_CONTRACTS.md](CROSS_FORK_CONTRACTS.md)** — 跨仓库私有契约（OpenGL 链路：shm 页 / present 协议 / 环境变量 / ready 标记 / 回调 / IPC；Vulkan 链路：surface tag / vn_winehua_present / VK_PRESENT 协议 / 设备释放回调；各含两端代码索引与失效表现）
- **[AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md)** — 音频架构
- **[DXVK_MODERN_UPGRADE_READINESS.md](DXVK_MODERN_UPGRADE_READINESS.md)** — DXVK 2.x/VKD3D 升级能力矩阵、迁移清单与准入门禁
- **[VKD3D_LIMITED_500K_PLAN.md](VKD3D_LIMITED_500K_PLAN.md)** - VKD3D 2.6 limited-500K official D3D12 profile, gates, and device evidence
- **[VKD3D_DESCRIPTOR_500K_910_20260804.md](VKD3D_DESCRIPTOR_500K_910_20260804.md)** — 910 设备 500K 末槽 GPU descriptor 三次真机证据
- **[VKD3D_BDA_910_20260805.md](VKD3D_BDA_910_20260805.md)** — 910 设备 BDA/GPUVA 三次真机证据
- **[VKD3D_GRAPHICS_SMOKE_910_20260805.md](VKD3D_GRAPHICS_SMOKE_910_20260805.md)** — 910 设备 VKD3D 物理显示 1000 帧三次真机证据
- **[VKD3D_READBACK_REVALIDATION_910_20260806.md](VKD3D_READBACK_REVALIDATION_910_20260806.md)** - persistent-map readback regression root cause and post-fix 910 revalidation checkpoint
- **[VKD3D_MULTIQUEUE_910_20260805.md](VKD3D_MULTIQUEUE_910_20260805.md)** — 910 设备 D3D12 COPY/DIRECT 跨队列同步三次真机证据
- **[VKD3D_DXVK_REGRESSION_910_20260805.md](VKD3D_DXVK_REGRESSION_910_20260805.md)** — VKD3D 隔离分支的 DXVK Legacy 全量回归与 Modern 独立失败记录

### 构建
- **[BUILD_GUIDE.md](BUILD_GUIDE.md)** — 构建步骤、产物说明
- **[BUILD_ENV.md](BUILD_ENV.md)** — 从零搭建构建环境

### 平台限制与已知问题
- **[NOEXEC_MMAP_ANALYSIS.md](NOEXEC_MMAP_ANALYSIS.md)** — noexec 文件系统上 mmap+PROT_EXEC 修复（已解决，设计依据）
- **[OHOS_MMAP_ANALYSIS.md](OHOS_MMAP_ANALYSIS.md)** — OHOS mmap 权限调研报告（平台硬约束）
- **[X86_64_PC_ISSUES.md](X86_64_PC_ISSUES.md)** — x86_64 PC 已知问题

### 工程维护
- **[SUBMODULE_MAINTAINABILITY.md](SUBMODULE_MAINTAINABILITY.md)** — submodule 合并风险现状与改进路线图（评审依据）
- **[CODE_IMPROVEMENT_PLAN.md](CODE_IMPROVEMENT_PLAN.md)** — 代码改进实施计划（SUBMODULE_MAINTAINABILITY 的执行篇，阶段 0-4）
- **[REMOTE_HDC.md](REMOTE_HDC.md)** — hdc 远程共享配置
- **automation/** — WSL 回归测试套件运行器（`automation/README.md`，纯 WSL，不写死环境路径）

### 归档（一次性报告/历史记录，不再维护）
- [virgl_display_optimization_guide.md](archive/virgl_display_optimization_guide.md) — 显示链路优化方案（已被 surface-queue zero-copy 实现超越）
- [PHASE2_DXVK_MERGE_REPORT.md](archive/PHASE2_DXVK_MERGE_REPORT.md) — Phase2 合并决策报告（合并已完成，PR #47）
- [DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md](archive/DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md) — 07-21 性能调查（结论已被 STATUS_MEMO 超越）
- [PHASE2_DXVK_STATUS_MEMO_sections_01-40.md](archive/PHASE2_DXVK_STATUS_MEMO_sections_01-40.md) — STATUS_MEMO 的历史调查章节
- [CPP_REFACTOR_PLAN.md](archive/CPP_REFACTOR_PLAN.md) — compositor 重构复盘（重构已 100% 完成）
- [BOX32_MMAP_PROBE.md](archive/BOX32_MMAP_PROBE.md) — Box32 32-bit mmap 探针（结论已落地）
- [WINE_MUSL_GLIBC_DIFF.md](archive/WINE_MUSL_GLIBC_DIFF.md) — musl 适配评估（musl 已是生产方案）

## 关键里程碑

| 日期 | 里程碑 |
|------|--------|
| 2026-06-12 | cmd.exe 在设备上运行 |
| 2026-06-13 | notepad.exe GUI headless 验证 |
| 2026-06-14 | NAPI 沙箱 + Wayland 渲染上屏 |
| 2026-06-15 | 多窗口架构 + 输入框架 |
| 2026-06-21 | ARM64 Pad Box64 .so 方案完成 |
| 2026-07-06 | 音频 Host Broker 引擎完成 |
| 2026-07-09 | VirGL / OpenGL guest Mesa 渲染完成 |
| 2026-07-13 | 渲染管线性能优化 (Native VSync, 合成签名, 缓冲复用) |
| 2026-07-25 | compositor 状态重构完成（feature/split-wayland-server） |
| 2026-07-28 | Phase 2 DXVK/Venus 合并门禁通过（profile: shadow-precise-dirty-ring-inline-upload-coverage-sort） |
| 2026-07-29 | D3D8 虚拟显示兼容合并 (PR #47) |
| 2026-07-30 | DXVK 1.10.3 stable baseline（910/920 双设备 dxvk suite 全 PASS） |
