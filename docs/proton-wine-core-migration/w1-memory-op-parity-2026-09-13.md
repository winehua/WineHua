# W1 内存操作对照：WineHua 原实现 → Proton 移植树

> 日期：2026-09-13
> 目的：按"参考原来的实现逻辑、特别是内存操作"的要求，把 WineHua 在 Wine 上做的
> **内存/地址空间相关改动**逐条与移植树比对，给出"已对齐 / 有差异 / 无落点"的判定。
> 基线：原实现 `WineHua-arm64ec/thirdparty/wine` @ `feature/arm64-heaven-port`（11.10）；
> 移植树 `WineHua-proton-ohos/thirdparty/wine-valve` @ `ohos-port`（Valve `proton_11.0`）。

## 1. 方法（可复跑）

```text
1) 提交层面对齐：把原实现的 WineHua 提交按 subject 与移植树提交一一配对
   （移植树 110 个 WineHua 提交，subject 全部有对应 → 提交层无遗漏）
2) 内容层面收敛：对每个提交的"新增行"逐行检查是否存在于移植树；
   再剔除"后来又被删掉的行"，只保留"原实现最终文件里仍然存在、移植树里找不到"的行
3) 手工复核每条候选：区分 真丢失 / 架构差异 / 仅改写 / 死代码
```

脚本（Windows 侧，经 WSL 运行）：`F:\WineHua\.codex-probe\15-loss-final-fast.sh`
中间结果：`/tmp/wh-probe/cand-live.tsv`（候选）、`/tmp/wh-probe/loss3.tsv`（缺失明细）

## 2. 内存/地址空间相关改动逐条判定

| # | 原实现的位置与逻辑 | 移植树状态 | 判定 |
| --- | --- | --- | --- |
| 1 | `dlls/ntdll/unix/ohos_virtual.{c,h}`：noexec 文件系统下的 `mprotect_exec` / `ohos_map_exec_section` / JIT 开关 / Box64 故障路由 | 文件内容与原件**逐行一致**（移植树只额外加了 SMC 路由判定） | **已对齐** |
| 2 | `virtual.c: anon_mmap_tryfixed()` 里 `#ifdef __OHOS__ / #undef MAP_FIXED_NOREPLACE`（Box64 包装的 mmap 不支持该 flag） | 移植树同位置存在同款守卫 | **已对齐** |
| 3 | `virtual.c: mprotect_exec()` → `ohos_mprotect_exec()`、`ohos_jit_enable()`、`map_image_into_view()` → `ohos_map_exec_section()` | 三处都在，行号不同（11.0 结构差异） | **已对齐** |
| 4 | `virtual.c: virtual_alloc_first_teb()` 的 `MEM_TOP_DOWN` OHOS 豁免 | 在 | **已对齐** |
| 5 | `virtual.c: virtual_alloc_thread_data()` 失败时回退 `anon_mmap_alloc()`（Box64 兼容） | 11.0 **没有** `virtual_alloc_thread_data()`：线程数据内嵌在 TEB（`&teb->GdiTebBatch`），内核栈走 `virtual_alloc_thread_stack()` → `map_view()` | **无落点**（不是丢失）。若将来 FEX/box64 下 `map_view` 申请内核栈失败，需在 11.0 的 `virtual_alloc_thread_stack()` 上补同义回退 |
| 6 | `virtual.c`: `USE_UFFD_WRITEWATCH` + `kernel_writewatch_init()` | 11.0 才引入 UFFD writewatch；移植树按平台禁用：`#if defined(UFFD_FEATURE_WP_ASYNC) && ... && !defined(__OHOS__)` | **移植树新增，平台必需**（原树没有这段代码可参考） |
| 7 | `server/mapping.c: init_memory()` 用 `#ifdef __aarch64__` 区分高地址区：aarch64 走探测式 `free_available_high_map_addr()`，其它架构走无条件 `free_map_addr()` | 移植树**无条件**调用 `free_available_high_map_addr()` | **等价但有差异**：arm64 目标上行为一致；非 aarch64 构建会偏离原逻辑 |
| 8 | `dlls/wow64/syscall.c`：`HODLL` 环境变量选 32 位转译器，ARM64 默认 `libwow64fex.dll` | 在（写成一行三元表达式） | **已对齐** |
| 9 | `dlls/ntdll/unix/signal_arm64.c`：SMC/CALLRET 先于 Wine SEH/DFX 路由 | 在（形参名不同） | **已对齐** |
| 10 | `dlls/win32u/vulkan.c`：`vkMapMemory` 别名进 32 位 VA（`wow64_alias` / `wow64_copy` 双路径 + CPU 拷贝兜底） | 与原实现**提交版**一致 | **已对齐**，但见 §3 |

## 3. 需要注意的两处差异（内存操作）

### 3.1 WoW64 Vulkan 映射：原实现工作区已有更安全的版本

- 移植树当前实现（= 原实现提交版 `fix(wow64): alias vkMapMemory into 32-bit VA`）：
  `mremap(DONTUNMAP)` / `mmap(fd)` 之后，用 `winehua_wow64_alias_coherent()` **向 host 与别名地址各写一个字节再回读**来判断别名是否真的共享页。
- 原实现工作区（未提交）已改为 `dlls/win32u/winehua_shared_map.h`：
  - 先按 `/proc/self/maps` 找 shared VMA，再 **优先 dup 活着的 BO fd**（`/proc/self/fd`），`map_files` 仅作兜底；
  - **不对 host VMA 做任何写探测**（注释明确：`the original host VMA is never moved or probed with writes`）；
  - 加 `WINEHUA_PERF_DIAGNOSIS=1` 的映射性能计数器。
- 判定：写探测对**设备内存（dma-buf / Venus BO）**有实打实的副作用风险，建议随原实现落地后同步到移植树，
  而不是移植树自己先发明一套。

### 3.2 `server/mapping.c` 的 aarch64 分支

建议按原实现补 `#ifdef __aarch64__`，理由：这是"高区探测"策略的开关，原实现明确记录
「探测式在 box64 转译下减半重试会破坏高区可用性，explorer 启动即崩 (SIGQUIT)」。
移植树无条件探测在 arm64 上等价，但保留了以后换架构时踩同一坑的可能。

## 4. 顺带发现（非内存，记录备查）

| 文件 | 现象 | 判定 |
| --- | --- | --- |
| `dlls/mmdevapi/client.c` | `backend_allows_shared_mode_conversion()` 在移植树里**只被定义、从未被调用** | 11.0 的共享模式格式判定走 `wine_unix_call(is_format_supported)`（由后端答复），
所以不是功能回归，属死代码；建议删除或接到 11.0 的判定路径上 |
| `dlls/ntdll/unix/loader.c` | 原实现的 `OHOS-DIAG` wow64 装载诊断日志不在移植树 | 原实现后续提交已自行删除，属未迁的调试代码 |
| `dlls/winewayland.drv/opengl_readback.{c,h}` | 函数签名 3 参（原）vs 4 参（移植树，含 `BOOL raw`） | **移植树正确**：必须匹配 11.0 的 `struct opengl_driver_funcs.p_surface_create` |

## 5. 结论

1. **移植没有丢失内存/地址空间逻辑**：原实现里所有 OHOS 侧的内存相关改动，要么已逐行对齐，
   要么因 11.0 架构不同而没有落点（已写明落点）。
2. 真正待决的两条是 **§3**：WoW64 Vulkan 别名的安全性升级、`mapping.c` 的架构分支。
3. 32 位转译器选择上，**原实现本身就以 FEX 为 ARM64 默认**（`HODLL`/`libwow64fex.dll`），
   移植树已对齐；此前应用层显式指定 box64 的写法应退回"让 Wine 决定"。
