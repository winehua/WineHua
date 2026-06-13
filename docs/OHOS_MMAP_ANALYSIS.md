# OHOS mmap 权限调研报告

## 测试环境

- **设备**: HAD-W32 (ARM64)
- **内核**: Linux 39-bit 地址空间
- **终端测试**: hdc shell, 无 app 沙箱
- **应用测试**: HonWine app, 带 `ALLOW_WRITABLE_CODE_MEMORY` + `DISABLE_CODE_MEMORY_PROTECTION`

## 对比结果

| # | 测试项 | 终端 (hdc shell) | App 沙箱 |
|---|--------|-----------------|----------|
| 1a | 匿名 `MAP_PRIVATE\|ANON` 全部 prot (NONE→RWX) | ✓ OK | ✓ OK |
| 1b | 匿名 `MAP_SHARED\|ANON` 全部 prot | ✓ OK | ✓ OK |
| 1c | 文件 `memfd\|SHARED` NONE/R/W/RW | ✓ OK | ✓ OK |
| 1c | 文件 `memfd\|SHARED` X/RX/WX/RWX | ✗ err=13 | ✗ err=13 |
| 1d | 文件 `memfd\|PRIV` NONE/R/W/RW | ✓ OK | ✓ OK |
| 1d | 文件 `memfd\|PRIV` X/RX/WX/RWX | ✗ err=13 | ✗ err=13 |
| 2 | 大尺寸匿名 RWX (64K→256M) | ✓ OK | ✓ OK |
| 3a | `MAP_FIXED` 匿名 RW/RX/RWX | ✓ OK | ✓ OK |
| 3b | `MAP_FIXED` 文件 RW | ✓ OK | ✓ OK |
| 3b | `MAP_FIXED` 文件 RX/RWX | ✗ err=13 | ✗ err=13 |
| 4 | `MAP_NORESERVE` 匿名 全部 prot | ✓ OK | ✓ OK |
| 5 | 高地址 hint (0/4G/252G/508G) RWX | ✓ OK | ✓ OK |
| 6 | `MAP_32BIT` (0x40) 任意 prot | ✗ err=22 | ✗ err=22 |
| 7a | mprotect 匿名 RW→全部 (含 RX/RWX) | ✓ OK | ✓ OK |
| 7b | mprotect 文件 RW→RX/RWX | ✗ err=13 | ✗ err=13 |
| 8 | 匿名 RWX + ARM64 ret 执行 | ✓ OK | ✓ OK |
| 9 | 匿名 RW + mprotect RX + 执行 (Dynarec 模式) | ✓ OK | ✓ OK |
| 10 | 相邻两页: RW(数据) + mprotect→RX(代码) + 执行 | ✓ OK | — |

## 关键结论

### 1. 终端和 App 沙箱结果完全一致

没有任何差异。说明 mmap 限制是 **OHOS 内核层面的通用约束**，与 app 沙箱无关。

### 2. 可用能力

- 匿名映射支持全部保护位组合，包括 RWX
- 大尺寸匿名 RWX 可达 256MB+
- `mprotect` 可以在匿名映射上自由切换权限
- 高地址 hint (含 47-bit) 均可被内核接受
- 匿名 RWX 映射上可正常执行 ARM64 代码

### 3. 不可用能力

| 操作 | 错误 | 影响 |
|------|------|------|
| 文件映射 + PROT_EXEC | EACCES (13) | memfd/文件-backed 可执行代码不可用 |
| mprotect 文件→EXEC | EACCES (13) | 无法给文件映射加执行权限 |
| `MAP_32BIT` flag | EINVAL (22) | ARM64 上此 x86_64 flag 不支持 |

### 4. 对 Box64/Wine 的影响

| Box64 组件 | 使用的 mmap 模式 | OHOS 兼容性 |
|-----------|-----------------|------------|
| Bridge 分配 (NewBrick) | 匿名 RWX, MAP_PRIVATE\|ANON | ✓ 可用 |
| ELF pre-allocation | 匿名 NONE, MAP_NORESERVE | ✓ 可用 |
| ELF 代码段 (文件映射) | 文件 MAP_FIXED + RX | ✓ 可用 (此路径通过文件 fd 直接 mmap，已证实工作) |
| ELF 代码段 (匿名回退) | 匿名 RW + read + mprotect→RX | ✓ 可用 |
| Dynarec 代码块 | 匿名 RW + mprotect→RX + 执行 | ✓ 可用 |
| box_mmap (find47bitBlock) | 匿名 RWX + 高地址 hint | ✓ 可用 |
| box_mmap (MAP_32BIT 路径) | 匿名 + MAP_32BIT flag | ✗ errno=22 |
| memfd workaround | 文件 SHARED + mprotect→RX | ✗ errno=13 |

### 5. Box64 需要修改的位置

**惟一的修复点**：阻止 Box64 在任何路径添加 `MAP_32BIT` flag。

具体做法：
- 设置环境变量 `BOX64_MMAP32=0` — 禁用 `mmap64` wrapper 中 `running32bits && BOX64ENV(mmap32)` 触发的 MAP_32BIT 自动添加
- 保持 bridge.c 原始代码不变（`box_mmap` 路径在 ARM64 OHOS 上正常工作）

**不需要修改的**：
- `find47bitBlock` / 高地址 hint — 已验证可用
- ELF loader — 文件映射 RX 已验证可用
- mprotect — 匿名映射上已验证可用
- memfd/syscall workaround — 不需要，原始路径已足够

## fork 深度与 PROT_EXEC

测试代码：`HonWine/entry/src/main/cpp/napi_init.cpp` `RunMmapTests`（NAPI 原生测试）和 `thirdparty/box64/src/main.c`（Box64 SELFTEST）。

### NAPI 原生测试 (HonWine app 进程树)

| 进程深度 | 测试项 | 结果 |
|---------|--------|------|
| 0 (APP 主进程) | ANON RWX 4KB-256MB | ✓ OK |
| 1 (fork 子进程) | ANON RWX 4KB | ✓ OK |
| 2 (嵌套 fork 孙进程) | ANON RW 4KB | ✓ OK |
| 2 (嵌套 fork 孙进程) | ANON RWX 4KB | ✓ OK |

**结论：原生 App 进程的 fork 深度不影响 mmap(RWX) 能力。** 孙进程（depth 2）和主进程一样可创建可执行内存。

### Box64 SELFTEST (Box64 内 fork)

Box64 进程本身是 App 的 fork+execve 子进程（depth 1）。

| 场景 | 进程 | 4K RWX | 128K RWX | 512K RWX | 2MB RWX |
|------|------|--------|----------|----------|---------|
| Box64 主进程 | depth 1 | OK | OK | OK | OK |
| Box64 fork 子进程 | depth 2 | FAIL EINVAL | FAIL EINVAL | FAIL EINVAL | FAIL EINVAL |

**结论：Box64 进程的 fork 子进程完全不能创建 PROT_EXEC 内存，跟 size 无关。**

### SPAWN-SELFTEST (my_posix_spawn fork 子进程, execve 前)

| 场景 | RW | RWX |
|------|-----|------|
| my_posix_spawn fork 子进程 (depth 2, execve 前) | OK | FAIL EINVAL |

### wineserver (Box64 孙进程, depth 2, execve 后)

所有 SELFTEST 全 FAIL（连 4K RWX 都不行），且 `mprotect(RW→RWX)` 也 FAIL。

### 对比总结

| 测试 | 深度 | RWX 结果 |
|------|------|----------|
| NAPI 原生 fork | 1 | OK |
| NAPI 原生嵌套 fork | 2 | OK |
| Box64 main SELFTEST | 1 | OK |
| Box64 main fork 子进程 | 2 | **FAIL EINVAL** |
| my_posix_spawn fork 子进程 | 2 | **FAIL EINVAL** |
| wineserver (spawn + execve) | 2 | **FAIL EINVAL** |

原生进程深度不影响 RWX，但 Box64 进程的 fork 子进程完全不能创建 RWX 内存。

### InternalMmap(NULL) vs box_mmap(hint)

Box64 内部有两种 mmap 调用路径：

| 路径 | 调用链 | 结果 |
|------|--------|------|
| `box_mmap(NULL, size, RWX)` | box_mmap → find47bitBlock(hint) → InternalMmap(hint, ...) | **OK** |
| `InternalMmap(NULL, size, RWX)` | syscall(__NR_mmap, NULL, ...) 直接调内核 | **FAIL EINVAL** |

Dynarec 分配代码(`AllocDynarecMap`, `MmaplistAddBlock`)原始走 `InternalMmap(NULL, ...)` 绕过 hint，导致 2MB RWX 分配失败。修改为 `box_mmap(NULL, ...)` 后通过 `find47bitBlock` hint 解决了此问题。
