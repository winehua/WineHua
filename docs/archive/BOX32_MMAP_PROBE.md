# Box32 低 4GB mmap 探针报告

> 状态：已完成（2026-09-22 整理归档）。Box32 的 32 位 mmap 探针，结论已落地。

> 测试日期: 2026-07-04
> 设备: Pad (ARM64), WineHua App
> 代码: `entry/src/main/cpp/wine_mmap_test.cpp` (Phase 2) + `wine_child.cpp` (MmapTestMain NCP 入口)

## 测试方式

两种环境分别测试：

| 环境 | 入口 | 说明 |
|------|------|------|
| App 主进程 | `runMmap32BitTests()` NAPI 调用 | 直接在主线程 mmap |
| **NCP 子进程** | `runMmapNcpTests()` → `OH_Ability_StartNativeChildProcess("libwine_child.so:MmapTestMain")` | 与 box32/box64 实际运行环境一致 |

## 结论 1: NCP 子进程低 4GB 完全干净

### 主进程
```
/proc/self/maps 低 4GB:
  00040000-00080000  rw-p  [anon:ark-Object Space]
  00080000-000c0000  rw-p  [anon:ark-Non Movable Space]
  000c0000-00100000  rw-p  [anon:ark-Object Space]
  00100000-10000000  rw-p  [anon]
  20000000-20040000  rw-p  [anon]
  → 5 regions, ARK 占 ~768KB [0x00040000, 0x00100000)
  → 0x01000000 和 0x10000000 被占用，MAP_FIXED 无法使用
```

### NCP 子进程 (box32 实际运行环境)
```
/proc/self/maps 低 4GB:
  → 0 regions, ARK=0
  → 完全空白，无任何占用
```

ARK 区域是鸿蒙方舟运行时的内部映射，仅在主进程存在。`OH_Ability_StartNativeChildProcess` 通过 appspawn fork 出的子进程**不继承 ARK 映射**，低 4GB 地址空间完全可用。

**结论**: box32 运行在 NCP 子进程中，**不需要 `unmap_low_anon_regions()`**，可直接用 `MAP_FIXED` 精确控制任意低地址。

### 低地址 hint 行为

```
NCP 子进程 hint 测试:
  hint=1MB    → @0x00100000  BELOW_4G  at_hint
  hint=16MB   → @0x01000000  BELOW_4G  at_hint
  hint=256MB  → @0x10000000  BELOW_4G  at_hint
  hint=512MB  → @0x20000000  BELOW_4G  at_hint
  hint=1GB    → @0x40000000  BELOW_4G  at_hint
  hint=2GB    → @0x80000000  BELOW_4G  at_hint
  hint=3.75GB → @0xF0000000  BELOW_4G  at_hint
  → 7/7 hints honored below 4GB, ALL at requested address
```

ARM64 OHOS 内核完全遵守低地址 hint，不会像某些 Linux 发行版那样忽略低地址 hint 改给高地址。

## 结论 2: MAP_FIXED_NOREPLACE 不存在

```
NCP 子进程:
  NOREPLACE conflict → MAP_FAILED(expected)  err=38,Function not implemented
  NOREPLACE free     → FAIL_or_wrong          err=38,Function not implemented
  NOREPLACE scan 176 个地址 → 0 free, 176 occupied (全部误报为"被占用")
```

| 项目 | 结果 |
|------|------|
| 错误码 | ENOSYS (38) — 内核未实现 |
| 影响 | 所有使用该 flag 的 mmap 调用直接失败 |
| 对 box32 的影响 | 当前 `box_mmap32_hard_search_ohos()` 完全无效，所有低地址扫描返回空 |

### 替代方案

不能用内核原子操作，必须用用户态实现：

1. 解析 `/proc/self/maps` 获取已用区间
2. 在自由区间内用 `MAP_FIXED` 精确分配
3. 加锁防并发冲突

## 结论 3: 基础环境全部可用

NCP 子进程中以下操作全部通过：

| 测试项 | 结果 | box32 依赖 |
|------|------|------|
| 256MB 连续低 4GB 分配 (`0x10000000`) | ✅ OK | heap 分配器 |
| `MAP_FIXED` 低地址 (`0x01000000`, `0x10000000`, `0x40000000`) | ✅ OK | 精确地址控制 |
| 匿名 RW→mprotect RX (Dynarec 模式) | ✅ OK | JIT 代码生成 |
| 匿名 RWX | ✅ OK | 直接可执行分配 |
| `MAP_32BIT` (0x40) | ❌ EINVAL | ARM64 不支持 |
| `mmap_min_addr` 可读性 | ❌ 不可读 | 无影响 |

box32 所需的所有内核 mmap 特性（除 MAP_FIXED_NOREPLACE 和 MAP_32BIT）均已在 NCP 子进程中验证通过。

## 对实现的指导

| 当前实现 | 问题 | 修正方向 |
|------|------|------|
| `box_mmap32_hard_search_ohos()` 用 NOREPLACE 扫描 | ENOSYS，永远返回空 | 改用 `/proc/self/maps` 解析 + `MAP_FIXED` |
| `box32_heap_init_locked()` 用 NOREPLACE 抢占 0x10000000 | 第一步失败，但 fallback 用 hint mmap 成功 | 去掉 NOREPLACE 分支，直接用 hint mmap（NCP 中 hint 精度 100%） |
| `unmap_low_anon_regions()` | NCP 中不需要 | 可移除或简化为空操作 |
| `MAP_32BIT` fallback | ARM64 永远不可用 | 必须走硬搜索替代路径 |
