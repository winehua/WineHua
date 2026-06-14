# Phase 2 预研: Wine on HarmonyOS ARM64

> **日期**: 2026-06-11
> **状态**: 预研阶段，ARM64 Unix 侧编译验证通过

---

## 1. 核心概念

**场景**: HarmonyOS ARM64 设备上运行 Windows x86_64 程序。

需要**两层翻译**：

| 层次 | 翻译方向 | 工具 |
|------|---------|------|
| CPU 指令 | x86_64 → ARM64 | FEX-Emu (ARM64EC ABI) |
| CPU 指令 | x86_32 → ARM64 | Box64 (WoW64 ABI) |
| OS API | Windows → POSIX | Wine (ARM64 原生, 不模拟) |

**关键**: Wine 本身以 ARM64 原生代码运行，只有 Windows 程序的 x86 指令在模拟器中执行。Windows API 调用时"跳出"模拟器，回到原生 ARM64 Wine。

**完整数据流**:
```
┌────────────────────────────────────────────────────┐
│          HarmonyOS ARM64 设备                       │
│                                                    │
│  Windows x86_64 .exe                               │
│      │                                             │
│      ▼                                             │
│  ┌──────────────┐   ARM64EC ABI    ┌────────────┐  │
│  │ FEX-Emu      │ ←───────────→   │ Wine ARM64 │  │
│  │ (x86_64→ARM) │   syscall 边界   │ (原生)      │  │
│  └──────────────┘                  └─────┬──────┘  │
│                                         │         │
│  ┌──────────────┐   WoW64 ABI          │         │
│  │ Box64        │ ←───────────→        │         │
│  │ (x86→ARM64)  │   syscall 边界       │         │
│  └──────────────┘                      │         │
│                                         ▼         │
│                              OHOS Kernel (Linux)   │
└────────────────────────────────────────────────────┘
```

**为什么用两个模拟器？**
- **FEX** 实现了 ARM64EC ABI（Microsoft 标准），支持 x86_64 代码与 ARM64 原生代码在同一进程内混合执行——这是 Hangover 性能优势的核心
- **Box64** 处理 32 位 x86 程序（很多老旧 Windows 软件和安装程序是 32 位）
- 两者都编译为 Wine PE DLL (`libarm64ecfex.dll` / `wowbox64.dll`)，在 Wine 进程内运行

**备选方案**: 是否可只用 Box64 同时处理 x86_64 和 x86？
- Box64 本身支持 x86_64 → ARM64 翻译（就是干这个的），但它未实现 ARM64EC ABI
- 如果放弃 ARM64EC（纯模拟 mode），性能会显著下降
- 但作为**第一阶段验证**，单用 Box64 更简单（Box64 OHOS 移植 已有 OHOS port）

---

## 2. 编译验证 (2026-06-11)

### 2.1 ARM64 wineserver ✅
```
file:  ELF 64-bit LSB pie executable, ARM aarch64
interpreter: /lib/ld-musl-aarch64.so.1
NEEDED: libc.so
size:  966K
```

### 2.2 ARM64 ntdll/unix ✅ (15/21 编译通过)

6 个失败是因缺少 configure 生成的头文件 (`ntsyscalls.h`, `locale_private.h`, `SYSTEMDLLPATH` 等)，非 musl 兼容性问题。

### 2.3 构建命令

```bash
TARGET=aarch64-linux-ohos
CFLAGS="--target=$TARGET --sysroot=$SYSROOT
    -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO
    -fPIC -I$WINE/include -I$WINE/include/wine"
```

**结论**: Phase 1 的 x86_64 编译配方对 ARM64 完全适用，只需改 target triple。

---

## 3. 已有资源

| 组件 | 状态 | 说明 |
|------|------|------|
| Box64 on OHOS ARM64 | ✅ | Box64 OHOS 移植, 14/14 测试通过 |
| OHOS ARM64 SDK | ✅ | `aarch64-linux-ohos` 完整可用 |
| Wine ARM64 Unix 侧 | 🟡 | 编译基本可行，需完整 configure |
| Wine ARM64 PE DLLs | ⬜ | 需要 `aarch64-w64-mingw32` 工具链 |

---

## 4. Phase 2 待解决问题

### 4.1 PE 交叉编译
- **问题**: 需要 `aarch64-w64-mingw32-clang` (llvm-mingw ARM64)
- **影响**: 阻塞完整 Wine ARM64 构建
- **方案**: 安装 llvm-mingw for aarch64, 或使用 Hangover 的 by-laws 分支

### 4.2 FEX-Emu 的 musl 移植
- **问题**: FEX-Emu 依赖 glibc 内部机制 (signal, TLS, dlopen)
- **当前状态**: 无已知 musl port
- **替代方案**: Box64 也可处理 x86_64? (目前 Box64 主要处理 32-bit)

### 4.3 ARM64EC ABI
- **问题**: ARM64EC 是 Microsoft 定义的 ABI，用于混合 ARM64 原生和 x86_64 模拟代码
- **Hangover 方案**: `libarm64ecfex.dll` (FEX 实现)
- **OHOS 适配**: 需要确认 OHOS kernel 支持 ARM64EC 所需的特性

### 4.4 图形栈
- **问题**: OHOS 无 X11/Wayland
- **Phase 1 策略**: 仅控制台
- **Phase 2 策略**: Mesa zink (Vulkan → OpenGL) + OHOS 原生窗口

---

## 5. 技术路线建议

### 路线 A: 简化版 (Box64 only for x86_64)

```
Phase 2a-1: 完整 Wine ARM64 编译
├── 获取 llvm-mingw aarch64 (或跳过 PE, 复用 Box64 OHOS 移植 的套路)
├── configure --host=aarch64-linux-ohos
├── 编译 ARM64 Unix .so (已完成验证)
└── 目标: wineserver + ntdll.so 在真机上运行

Phase 2a-2: Box64 standalone 集成
├── Box64 OHOS 移植 已可用
├── 用 Box64 直接运行 x86_64 Wine (整套 Wine 编译为 x86_64)
│   即: Box64 模拟整个 x86_64 Wine 进程
├── 优点: 无需 ARM64EC, 无需 FEX port, 立即可用
└── 缺点: 性能较低 (整个 Wine 都在模拟器中)

Phase 2a-3: Box64 as Wine PE DLL
├── 编译 wowbox64.dll (Box64 编译为 Wine 内嵌 DLL)
├── Wine ARM64 原生 + Box64 只模拟 app 代码
└── 性能优于全 Wine 模拟
```

### 路线 B: 完整版 (FEX + ARM64EC)

---

## 6. 关键依赖和下载

| 工具 | URL | 用途 |
|------|-----|------|
| llvm-mingw (ARM64) | github.com/mstorsjo/llvm-mingw | PE 交叉编译 |
| by-laws llvm-mingw | github.com/bylaws/llvm-mingw | ARM64EC PE 交叉编译 |
| Box64 OHOS 移植 | 见第三方参考 | 20+ patch, 14/14 测试通过 |
| Hangover | github.com/AndreRH/hangover | Wine+Emu 集成方案 |
| FEX-Emu | github.com/FEX-Emu/FEX | x86_64→ARM64 模拟 |
