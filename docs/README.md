# Wine for HarmonyOS — 技术方案文档集

## 项目概述

将 Wine 移植到 HarmonyOS (OpenHarmony)，使 Windows 程序能在鸿蒙系统上运行。

## 分阶段路线

| 阶段 | 目标架构 | CPU 模拟 | 核心挑战 | 文档 |
|------|---------|----------|---------|------|
| **Phase 1** | x86_64 | 不需要 | musl libc 兼容性 | [WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md) |
| **Phase 2** | ARM64 | 需要 (Box64 + FEX) | musl + CPU 模拟 + 交叉架构 | (待 Phase 1 完成后) |

## 文档索引

### 核心文档
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — 整体架构设计，包括 Wine 内部架构、wineserver、PE→Unix 桥接
- **[WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md)** — Wine 对 glibc 的依赖分析，musl 移植的逐项适配清单
- **[UNCERTAINTIES.md](UNCERTAINTIES.md)** — 所有不确定点、风险和技术假设

### 参考项目
- **[Box64 OHOS 移植](https://参考文档)** — Box64 鸿蒙移植（20+ patch，14/14 测试通过）
- **[Hangover](https://github.com/AndreRH/hangover)** — Wine + Box64/FEX 集成（Phase 2 参考）
- **[Wine 上游](https://github.com/wine-mirror/wine)** — Wine 官方镜像

## 关键结论

### Phase 1 (x86_64) 可行性：✅ 已通过编译验证

**2026-06-11 里程碑**: wineserver 已成功交叉编译到 OHOS x86_64 musl。

| 验证项 | 结果 |
|--------|------|
| OHOS SDK x86_64 目标支持 | ✅ `x86_64-linux-ohos` 完整可用 |
| `arch_prctl` syscall | ✅ 头文件完整 (`ARCH_SET_GS/FS`, `__NR_arch_prctl=158`) |
| wineserver 编译 | ✅ 全部 44 个 .o 编译通过，0 错误 |
| wineserver 链接 | ✅ 产物: ELF x86-64, interpreter `ld-musl-x86_64.so.1` |
| Native Wine 工具链 | ✅ 完整构建可用 |

Wine 对 glibc 的**偶合度远低于 Box64**，实际编译验证仅需处理：
- 4 个缺失的 Linux 头文件 (ntsync, ipx, irda, ucdrom)
- 1 个 musl 缺失函数 (`epoll_pwait2` → stub 返回 ENOSYS)
- 0 个 `RTLD_NEXT` / `__libc_malloc` / `dlinfo` 使用

预估工作量：**1-2 个月** (1 人)，约 10-15 个 patch。详见 [BUILD_LOG.md](BUILD_LOG.md)。

### Phase 2 (ARM64) 可行性：✅ Unix 侧编译验证通过

ARM64 wineserver + ntdll 已成功交叉编译 (2026-06-11)，与 x86_64 使用相同配方。
详细预研见 [PHASE2_ARM64_RESEARCH.md](PHASE2_ARM64_RESEARCH.md)。

## 文档索引

### 核心文档
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — 整体架构设计
- **[WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md)** — glibc → musl 逐项适配清单
- **[UNCERTAINTIES.md](UNCERTAINTIES.md)** — 风险清单（含已解决的 P0 和新发现）
- **[BUILD_LOG.md](BUILD_LOG.md)** — 编译验证记录，环境配置，已知问题和修复

### 参考项目
- **[Box64 OHOS 移植](https://参考文档)** — Box64 鸿蒙移植（20+ patch，14/14 测试通过）
- **[Hangover](https://github.com/AndreRH/hangover)** — Wine + Box64/FEX 集成（Phase 2 参考）
- **[Wine 上游](https://github.com/wine-mirror/wine)** — Wine 官方镜像

## 项目结构

```
wineohos/
├── out/wineserver                  ← OHOS x86_64 二进制产物 ✅
├── scripts/
│   ├── build_wine_ohos.sh          ← 构建脚本
│   └── patches.sh                  ← 6 个 musl 适配 patch
├── docs/                           ← 技术方案文档集
│   ├── README.md
│   ├── ARCHITECTURE.md
│   ├── WINE_MUSL_GLIBC_DIFF.md
│   ├── UNCERTAINTIES.md
│   └── BUILD_LOG.md
└── .temp/                          # 研究用的 clone（不入库）
    ├── wine/                       # Wine 源码 + build-native + build-ohos
    ├── box64/
    ├── box64-ohos-ref/
    └── hangover/
```
