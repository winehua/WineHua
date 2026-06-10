# Wine for HarmonyOS — 项目计划

> **开始日期**: 2026-06-10
> **当前状态**: Phase 1 编译验证通过 ✅
> **最后更新**: 2026-06-11

---

## 总体路线图

```
Phase 1: x86_64 (musl 适配)          Phase 2: ARM64 (交叉架构)
┌─────────────────────────┐          ┌─────────────────────────┐
│ Wine on OHOS x86_64     │          │ Wine + Box64 + FEX      │
│                         │          │ on OHOS ARM64           │
│ ✅ 工具链验证            │          │                         │
│ ✅ wineserver 编译       │          │ ⬜ Box64 OHOS (已有)    │
│ ⬜ ntdll.so 编译         │          │ ⬜ FEX OHOS port        │
│ ⬜ 完整 Unix .so 编译    │          │ ⬜ ARM64EC PE DLLs      │
│ ⬜ 真机运行测试          │          │ ⬜ 图形栈适配            │
│ ⬜ HNP 打包              │          │                         │
└─────────────────────────┘          └─────────────────────────┘
    1-2 个月 (1人)                        3-6 个月 (1人)
```

---

## Phase 1 详细计划

### 第一阶段: 工具链和可行性验证 ✅ (已完成 2026-06-11)

| 任务 | 状态 | 产出 |
|------|------|------|
| OHOS SDK 环境分析 | ✅ | 确认 x86_64-linux-ohos 目标可用 |
| P0 不确定性验证 | ✅ | arch_prctl 头文件确认, SDK 路径确认 |
| Hello World 交叉编译 | ✅ | 工具链链路验证通过 |
| Native Wine 构建 | ✅ | 获得 winebuild, widl, wrc 等工具 |
| wineserver OHOS 编译 | ✅ | 44/44 .o 编译通过, 成功链接 |

### 第二阶段: 核心组件编译 (进行中)

| 任务 | 状态 | 预计工作量 |
|------|------|-----------|
| ntdll.so OHOS 编译 | ⬜ | 2-3 天 |
| 其他 Unix .so 编译 | ⬜ | 3-5 天 |
| PE DLLs 整合 (从 native build) | ⬜ | 1 天 |
| clang WARN_FLAGS 清理 | 🟡 | 1 天 |

### 第三阶段: 运行验证

| 任务 | 状态 | 预计工作量 |
|------|------|-----------|
| wineserver 真机运行 | ⬜ | 需要 OHOS x86_64 环境 |
| cmd.exe / wineconsole | ⬜ | 2-3 天 |
| 基础 Windows 程序测试 | ⬜ | 1 周 |
| musl 兼容性回归测试 | ⬜ | 3-5 天 |

### 第四阶段: 工程化

| 任务 | 状态 | 预计工作量 |
|------|------|-----------|
| patch 脚本完善 | 🟡 | 2-3 天 |
| 自动化构建脚本 | 🟡 | 2-3 天 |
| HNP 打包 | ⬜ | 1-2 天 |
| CI/CD pipeline | ⬜ | 1 周 |

---

## Phase 2 预计划 (ARM64)

| 阶段 | 任务 | 依赖 |
|------|------|------|
| 2.1 | Box64 OHOS 验证 (已有参考) | Phase 1 完成 |
| 2.2 | FEX-Emu OHOS port | 2.1 |
| 2.3 | ARM64EC PE 交叉编译环境 | 2.2 |
| 2.4 | Hangover 整合测试 | 2.3 |
| 2.5 | 图形栈 (Mesa/Vulkan on OHOS) | 2.4 |

---

## 风险和缓解措施

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| OHOS x86_64 真机环境不可用 | 中 | 高 | 优先 ARM64 真机; x86_64 用 QEMU 模拟器验证 |
| wineserver 运行时 OC bug | 中 | 高 | 参考 Box64 OHOS 移植 的 ABI 测试 |
| PE DLLs 与 OHOS musl 不兼容 | 低 | 中 | PE DLLs 是 x86_64 PE 格式，架构无关 |
| 图形栈需要 Wayland/X11 | 低 | 中 | Phase 1 仅控制台; 后续用 Mesa zink |
| OHOS 更新导致 API breaking | 低 | 中 | 锁定 SDK 版本; 定期更新适配 |

---

## 决策记录

| 日期 | 决策 | 原因 |
|------|------|------|
| 2026-06-10 | Phase 1 仅 x86_64 | 避免 CPU 模拟复杂度，先验证 musl 兼容性 |
| 2026-06-10 | 使用 Wine 官方镜像 (github.com/wine-mirror/wine) | GitHub 访问比 GitLab 稳定 |
| 2026-06-11 | 用 gcc 处理 PE, clang 处理 Unix | 避免 -mabi=ms 交叉编译问题 |
| 2026-06-11 | 只用 native PE DLLs | Phase 1 x86_64 PE DLLs 可复用，无需交叉编译 |

---

## 技术债务

| 项目 | 说明 | 优先级 |
|------|------|--------|
| clang WARN_FLAGS | 7 个 gcc 特有 warning flags 需要 cleanup | P3 |
| configure.ac OHOS patch | 添加 `linux-ohos*` host triple 支持 | P2 |
| Makefile musl_compat 集成 | 将 musl_compat.o 加入 Makefile 依赖 | P1 |
| vkd3d program_invocation_name | compile-time patch 待验证 | P1 |
| dll_soinit.c musl fallback | runtime patch 待验证 | P1 |
