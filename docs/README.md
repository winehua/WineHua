# WineHua 文档索引

> 更新日期: 2026-06-26

## 先看这些

- [ONBOARDING.md](ONBOARDING.md): 新机器或第一次接手仓库时的最短上手路径
- [BUILD_GUIDE.md](BUILD_GUIDE.md): 当前推荐的构建、打包、安装、抓日志方式
- [CURRENT_STATUS.md](CURRENT_STATUS.md): 当前能力、限制、默认验证目标
- [ARCHITECTURE.md](ARCHITECTURE.md): WineHua 总体架构
- [AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md): 音频链路设计
- [OPENGL_VIRGL_DESIGN.md](OPENGL_VIRGL_DESIGN.md): OpenGL / VirGL Step 1 设计、现状、待办、代码改动归档

## 当前默认入口

- Windows 宿主统一入口: `scripts/rebuild_harmony.ps1`
- 默认 backend: `MSYS2`
- fallback backend: `WSL`
- guest 3D receiver 构建: `scripts/build_ohos_guest_gfx.sh`
- guest 3D receiver 打包: `scripts/build_guest_gfx.sh`

## 与图形相关的重点结论

- 当前 OpenGL 跑通方式是复用 Wine 现有 `WGL/EGL + winewayland.drv` 路径
- 新增的是 guest Mesa `virpipe` runtime、宿主 `virgl_test_server` 启动管理，以及诊断 / 冒烟测试
- 当前最终显示仍复用现有嵌入式 Wayland compositor 上屏链路
- 当前不是零拷贝显示，不是多窗口专项方案，也还没有 DX/Vulkan 的最终结论

## 其他文档

- [MSYS2_BUILD_SYSTEM.md](MSYS2_BUILD_SYSTEM.md): MSYS2 构建体系
- [SOURCE_MANAGEMENT.md](SOURCE_MANAGEMENT.md): `thirdparty/` 子工程、graphics fork、源码归属管理
- [NOEXEC_MMAP_ANALYSIS.md](NOEXEC_MMAP_ANALYSIS.md): `noexec` 问题分析
- [OHOS_MMAP_ANALYSIS.md](OHOS_MMAP_ANALYSIS.md): OHOS `mmap` 行为研究
- [WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md): musl / glibc 差异记录
- [UNCERTAINTIES.md](UNCERTAINTIES.md): 风险和未决问题
