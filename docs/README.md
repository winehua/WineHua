# WineHua 文档索引

> 更新日期: 2026-06-24

## 先看这些

- [ONBOARDING.md](ONBOARDING.md): 新机器或第一次接手仓库时的最短上手路径
- [BUILD_GUIDE.md](BUILD_GUIDE.md): 当前推荐的构建、打包、安装、日志流程
- [MSYS2_BUILD_SYSTEM.md](MSYS2_BUILD_SYSTEM.md): MSYS2 构建体系、脚本职责、SDK overlay、DevEco 直构建
- [CURRENT_STATUS.md](CURRENT_STATUS.md): 当前能力、主线目标、已知限制
- [ARCHITECTURE.md](ARCHITECTURE.md): 代码结构、Wayland compositor、NAPI 与窗口模型

## 当前默认构建入口

- Windows 宿主统一入口: `scripts/rebuild_harmony.ps1`
- 默认 backend: `MSYS2`
- fallback backend: `WSL`
- 首次准备 MSYS2: `scripts/bootstrap_msys2.ps1`
- 底层单步入口: `build.sh`

推荐顺序是先看 `ONBOARDING.md`，再用 `rebuild_harmony.ps1`；只有在需要拆解单步排查时才直接调用 `build.sh`。

## 迁移后的心智模型

- `scripts/rebuild_harmony.sh` 仍然是唯一的内层编排入口
- `deps -> wine -> native -> hnp -> hap` 必须在同一个 shell 内完成
- `MSYS2` 负责 bash / pacman / autotools / meson / cmake / ninja 等宿主工具
- DevEco / OpenHarmony SDK、`hvigorw`、`hnpcli`、`hdc` 继续复用 Windows 侧安装
- DevEco IDE 直接构建会通过 `hvigorfile.ts` + `scripts/deveco_hvigor_env.js` 自动补齐 SDK overlay / Java / Node 环境
- `WSL` 还保留着，主要用于回归验证和应急 fallback

## 其他文档

- [SOURCE_MANAGEMENT.md](SOURCE_MANAGEMENT.md): `thirdparty/` submodule 管理
- [NOEXEC_MMAP_ANALYSIS.md](NOEXEC_MMAP_ANALYSIS.md): `noexec` 与可执行映射问题分析
- [OHOS_MMAP_ANALYSIS.md](OHOS_MMAP_ANALYSIS.md): OHOS `mmap` 行为研究
- [WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md): musl / glibc 差异记录
- [UNCERTAINTIES.md](UNCERTAINTIES.md): 历史风险和未决问题
