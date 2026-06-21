# Wine for HarmonyOS — 技术文档

## 项目概述

将 Wine 移植到 HarmonyOS (OpenHarmony)，使 Windows 程序在鸿蒙系统上运行。

当前架构：Wine (x86_64, musl) + Box64 (ARM64) → Wayland compositor → 鸿蒙 XComponent 上屏。

## 文档索引

### 当前状态
- **[CURRENT_STATUS.md](CURRENT_STATUS.md)** — 里程碑、已修复问题、源码修改清单、HNP 布局

### 架构与构建
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — Wine 内部架构、Wayland compositor 设计
- **[BUILD_GUIDE.md](BUILD_GUIDE.md)** — 构建环境、步骤、产物说明

### 技术分析
- **[WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md)** — glibc → musl 逐项适配分析
- **[NOEXEC_MMAP_ANALYSIS.md](NOEXEC_MMAP_ANALYSIS.md)** — noexec 文件系统上 mmap+PROT_EXEC 问题深度分析
- **[OHOS_MMAP_ANALYSIS.md](OHOS_MMAP_ANALYSIS.md)** — OHOS mmap 权限调研报告

### 规划与预研
- **[UNCERTAINTIES.md](UNCERTAINTIES.md)** — 剩余技术风险和待解决问题
- **[PHASE2_ARM64_RESEARCH.md](PHASE2_ARM64_RESEARCH.md)** — ARM64 交叉架构预研
- **[SOURCE_MANAGEMENT.md](SOURCE_MANAGEMENT.md)** — Wine/Box64 源码管理 (fork + submodule)

## 关键里程碑

| 日期 | 里程碑 |
|------|--------|
| 2026-06-11 | wineserver 交叉编译通过 |
| 2026-06-12 | cmd.exe 在设备上运行 |
| 2026-06-13 | notepad.exe GUI headless 验证 |
| 2026-06-14 | NAPI 沙箱 + Wayland 渲染上屏 |
| 2026-06-15 | multiton 多窗口架构 + 输入框架 |
| 2026-06-18 | 键盘输入修复 (XKB 数据打包 + 完整链路) |
| 2026-06-21 | ARM64 Pad Box64 .so 方案完成，prctl SIGSEGV 修复 |
| 2026-06-22 | Box64/Wine 日志收敛、环境变量重命名、Terminal 清理 |
