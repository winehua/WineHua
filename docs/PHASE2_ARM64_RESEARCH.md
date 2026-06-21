# ARM64 Pad 方案: Box64 .so 跨架构模拟

> 更新: 2026-06-22
> 状态: ✅ 已实现

---

## 1. 核心概念

**场景**: HarmonyOS ARM64 Pad 设备上运行 Windows x86_64 程序。

方案：**Box64 编译为共享库 (box64.so)** — Wine 编译为 x86_64 (musl)，Box64 .so 由 NCP 子进程 dlopen 加载，`box64_hmos_main()` 在同一进程内模拟执行 x86_64 Wine ELF。

```
┌────────────────────────────────────────────────────┐
│          HarmonyOS ARM64 Pad                        │
│                                                    │
│  NCP 子进程 (appspawn)                              │
│    wine_child.so: Main()                           │
│      dlopen("box64.so") → box64_hmos_main()        │
│        ↓                                           │
│  Wine x86_64 ELF + PE DLLs (rawfile zip 解压)      │
│        ↓ Box64 (x86_64 → ARM64 Dynarec)            │
│        ↓ winewayland.drv                           │
│  嵌入式 Wayland compositor → XComponent            │
└────────────────────────────────────────────────────┘
```

编译：`cmake -DLIBBOX64_SO=ON`，产物 `box64.so` 放入 `entry/libs/arm64-v8a/`。  
运行时：`wine_child.cpp` 中 `setenv("USE_LIBBOX64", "1")` 通知 Wine 侧适配 entryParams。
wineserver 编为 x86_64 PIE，同样由 Box64 加载，保持与 Wine 架构一致。

---

## 2. 编译验证 (2026-06-11)

ARM64 wineserver + ntdll/unix 已成功交叉编译：

| 组件 | 结果 |
|------|------|
| wineserver | ✅ ARM aarch64, `ld-musl-aarch64.so.1` |
| ntdll/unix | ✅ 15/21 编译通过 (6 个需 configure 头文件) |

Phase 1 的 x86_64 编译配方对 ARM64 完全适用，只需改 target triple。

---

## 3. 已有资源

| 组件 | 状态 | 说明 |
|------|------|------|
| Box64 on OHOS ARM64 | ✅ | Dynarec 模式正常运行 |
| OHOS ARM64 SDK | ✅ | `aarch64-linux-ohos` 完整可用 |
| Wine x86_64 on Box64 | ✅ | 当前生产路径 |
| Wayland compositor | ✅ | ARM64 原生编译 |
| XKB 键盘数据 | ✅ | 架构无关，已打包到 HNP |

---

## 4. 待解决问题

### Box64 fork 子进程 RWX 失败
- **问题**: Box64 fork 子进程无法创建 PROT_EXEC 内存 (EINVAL)
- **影响**: wineserver 等子进程受限制
- **缓解**: wineserver 使用 NAPI 原生 fork 绕过

### Wine ARM64 原生 (未来)
- 如需 Wine ARM64 原生编译，需要 `aarch64-w64-mingw32` 工具链交叉编译 PE DLL
- 当前 x86_64 + Box64 方案已满足基本需求，ARM64 原生暂不必要

---

## 5. 技术决策

| 日期 | 决策 | 原因 |
|------|------|------|
| 2026-06 | 使用 Box64 全模拟而非 ARM64 原生 Wine | Box64 OHOS 已有成熟移植，立即可用 |
| 2026-06 | Wayland compositor 用 ARM64 原生 | 性能关键路径，需与 XComponent 直接交互 |
