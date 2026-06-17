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

### 1. 终端和 App 沙箱结果一致

mmap 限制是 **OHOS 内核层面的通用约束**，与 app 沙箱无关。

### 2. 可用能力

- 匿名映射支持全部保护位组合，包括 RWX
- 大尺寸匿名 RWX 可达 256MB+
- `mprotect` 可以在匿名映射上自由切换权限
- 高地址 hint (含 47-bit) 均可被内核接受

### 3. 不可用能力

| 操作 | 错误 | 影响 |
|------|------|------|
| 文件映射 + PROT_EXEC | EACCES (13) | 文件-backed 可执行代码不可用 |
| mprotect 文件→EXEC | EACCES (13) | 无法给文件映射加执行权限 |
| `MAP_32BIT` flag | EINVAL (22) | ARM64 上此 x86_64 flag 不支持 |

### 4. 对 Box64/Wine 的影响

| Box64 组件 | 使用的 mmap 模式 | OHOS 兼容性 |
|-----------|-----------------|------------|
| Dynarec 代码块 | 匿名 RW + mprotect→RX | ✓ 可用 |
| ELF 代码段 (文件映射) | 文件 MAP_FIXED + RX | ✓ 可用 |
| ELF 代码段 (匿名回退) | 匿名 RW + pread + mprotect→RX | ✓ 可用 |
| box_mmap | 匿名 RWX + 高地址 hint | ✓ 可用 |
| MAP_32BIT 路径 | 匿名 + MAP_32BIT | ✗ EINVAL |

### 5. fork 深度与 PROT_EXEC

| 测试 | 深度 | RWX 结果 |
|------|------|----------|
| NAPI 原生 fork | 1 | OK |
| NAPI 原生嵌套 fork | 2 | OK |
| Box64 main SELFTEST | 1 | OK |
| Box64 main fork 子进程 | 2 | **FAIL EINVAL** |
| wineserver (spawn + execve) | 2 | **FAIL EINVAL** |

原生进程深度不影响 RWX，但 Box64 进程的 fork 子进程完全不能创建 RWX 内存。wineserver 使用 NAPI 原生 fork 绕过此限制。
