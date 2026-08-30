# WineHua D3D 帧率差异 — 环境变量判定与排查手册

> 生成: 2026-08-30. 设备: 192.168.1.8:33363 (arm64 Pad, Maleoon GPU, venus/virgl 链路).
> 数据来源: NCP 发射快照 (wine_child.cpp / virgl_child.cpp `dump_proc_snapshot`),
> 原始记录 `.temp/proc_baseline/proc_snapshot.log` 与 `proc_snapshot_1012.log`.
> 关联文档: WINE_PROC_ENV_BASELINE.md (90edaae 基线, 含高/低帧率完整 env 附录 E),
> WINE_PROC_ENV_BASELINE_1.0.12.md (main-ui 1.0.12 采集).

## 0. 结论摘要 (一句话)

同程序 (winehua_d3d_switch_cube.exe) 帧率高低由 **「vkd3d_limited_500k 混合路由所带的一套 venus
影子内存/强环机制组」× 「DXVK 1.10.3 (legacy)」 的组合失配** 决定: 两条件同真 → 低帧率;
任一不满足 → 高帧率。DXVK 1.10.3 本身不慢, 快速/慢速与启动路径 (应用库 vs 文件管理) 无关。

## 1. 判定矩阵

| # | 样本 | 代码位点 | 启动路径 | 路由/机制组 | DXVK | 帧率 |
|---|---|---|---|---|---|---|
| A | pid 32079 | 90edaae | 应用库 | vkd3d 混合 + 机制组 (有) | **modern-2.6** (2.6.2) | **高** |
| B | pid 62819 | 90edaae | 应用库 + 全局档 legacy | vkd3d 混合 + 机制组 (有) | legacy (1.10.3) | 低 |
| C | pid 33509 | 90edaae | 文件管理 (explorer 双击) | vkd3d 混合 + 机制组 (有) | legacy (1.10.3) | 低 |
| D | pid 53199 | main-ui 1.0.12 | 应用库 | 纯 dxvk (无机制组) | legacy (1.10.3) | **高** |

## 2. 两条渲染路由 (判断帧率的前提概念)

### 路由 X: vkd3d_limited_500k 混合路由 (90edaae/当前产品默认)
```text
D3D12 → vkd3d-proton 2.6 (limited-500k overlay)  ; D3D11/DXGI → DXVK(独立档位)
wine env 注入 (wine_env.cpp AppendD3dBackendEnv 的 vkd3d_limited_500k 分支):
  + WINEHUA_D3D_BACKEND=vkd3d_limited_500k
  + WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n        ← d3d12=n 是混合标志
  + WINEHUA_VKD3D_ROOT/PROFILE/… + WINEDLLDIR0=vkd3d/limited-500k/x64
  + venus 影子/强环机制组: 见 §3
```

### 路由 Y: 纯 DXVK 路由 (1.0.12 时代)
```text
D3D11/DXGI → DXVK (无 vkd3d overlay)
  + WINEHUA_D3D_BACKEND=dxvk_* (如 dxvk_legacy)
  + WINEDLLOVERRIDES=d3d11=n;dxgi=n               ← 无 d3d12=n
  + WINEDLLDIR0=dxvk/<profile>/x64
  + 无 WINEHUA_VKD3D_* / 无 VN_WINEHUA_* / 无 VKR_WINEHUA_SHADOW_FROM_HOST
  图形栈: 经典 vtest socket 直传 (WINEHUA_VTEST_PRESENT=surface-queue /
  VTEST_SOCKET_NAME=…virgl.sock / WINEHUA_GRAPHICS_BACKEND=virgl)
```

## 3. 【核心】环境变量判别表

### 3.1 机制组必备键 (同时出现才构成"机制组在")

- `WINEHUA_D3D_BACKEND` = `vkd3d_limited_500k`
- `WINEHUA_VKD3D_ROOT` = `…/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_PROFILE` = `limited-500k`
- `WINEHUA_VKD3D_VERSION` = `2.6`
- `WINEDLLOVERRIDES` = `d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLDIR0` = `…/wine/vkd3d/limited-500k/x64`
- `VN_WINEHUA_STRONG_RING_BARRIER` = `1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC` = `1`
- `VN_WINEHUA_DIRECT_FENCE_WAIT` = `1`
- `VKR_WINEHUA_SHADOW_FROM_HOST` = `precise`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC` = `1`
- `WINEHUA_PERF_PROFILE` = `shadow-precise`
- `VN_PERF` = `no_fence_feedback,no_query_feedback,no_multi_ring (无 no_semaphore_feedback; only in vkd3d 分支)`

### 3.2 DXVK 档位键

- `WINEHUA_DXVK_PROFILE` = `modern-2.6 | legacy`
- `WINEHUA_DXVK_VERSION` = `2.6.2 | 1.10.3`
- `WINEHUA_DXVK_ROOT` = `…/wine/dxvk/modern-2.6 | …/wine/dxvk/legacy`
- `WINEHUA_DXVK_RELAXED_FEATURES` = `1 (仅 legacy, DXVK 侧)`
- `DXVK_WINEHUA_BATCH_MAPPED_FLUSH 等 4 件` = `1/auto (legacy 兼容组, 但**不是慢因子** — 见 §5)`

### 3.3 帧率预测表 (组合)

| 机制组 (vkd3d 混合) | DXVK | 实测/推断帧率 |
|---|---|---|
| 有 | modern-2.6 (2.6.2) | 🟢 高 (实测 A) |
| 有 | legacy (1.10.3) | 🔴 低 (实测 B/C) ← 失配组合 |
| 无 (纯 DXVK 路由) | legacy (1.10.3) | 🟢 高 (实测 D) |
| 无 (纯 DXVK 路由) | modern-2.6 | 🟢 高 (推断; 与 1.0.12 上同理) |

## 4. 四样本判别值全表 (30 维)

| 键 | A 高 | B 低 | C 低 | D 高 |
|---|---|---|---|---|

| `WINEHUA_D3D_BACKEND` | `vkd3d_limited_500k` | `vkd3d_limited_500k` | `vkd3d_limited_500k` | `dxvk_legacy` |
| `WINEHUA_VKD3D_ROOT` | `…wine/vkd3d/limited-500k` | `…wine/vkd3d/limited-500k` | `…wine/vkd3d/limited-500k` | `—` |
| `WINEHUA_VKD3D_PROFILE` | `limited-500k` | `limited-500k` | `limited-500k` | `—` |
| `WINEDLLOVERRIDES` | `d3d12=n;d3d11=n;dxgi=n` | `d3d12=n;d3d11=n;dxgi=n` | `d3d12=n;d3d11=n;dxgi=n` | `d3d11=n;dxgi=n` |
| `WINEDLLDIR0` | `…wine/vkd3d/limited-500k/x64` | `…wine/vkd3d/limited-500k/x64` | `…wine/vkd3d/limited-500k/x64` | `…wine/dxvk/legacy/x64` |
| `WN_STRONG_RING_BARRIER` | `—` | `—` | `—` | `—` |
| `VN_WINEHUA_STRONG_RING_BARRIER` | `1` | `1` | `1` | `—` |
| `VN_WINEHUA_PERSISTENT_MAP_SYNC` | `1` | `1` | `1` | `—` |
| `VN_WINEHUA_REMOTE_MEMORY_SYNC` | `1` | `1` | `1` | `1` |
| `VN_WINEHUA_DIRECT_FENCE_WAIT` | `1` | `1` | `1` | `—` |
| `VKR_WINEHUA_SHADOW_FROM_HOST` | `precise` | `precise` | `precise` | `—` |
| `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC` | `1` | `1` | `1` | `—` |
| `WINEHUA_PERF_PROFILE` | `shadow-precise` | `shadow-precise` | `shadow-precise` | `—` |
| `VN_PERF` | `no_fence_feedback,no_query_feedback,no_semaphore…` | `no_fence_feedback,no_query_feedback,no_multi_rin…` | `no_fence_feedback,no_query_feedback,no_multi_rin…` | `no_fence_feedback,no_query_feedback` |
| `WINEHUA_DXVK_PROFILE` | `modern-2.6` | `legacy` | `legacy` | `legacy` |
| `WINEHUA_DXVK_VERSION` | `2.6.2` | `1.10.3` | `1.10.3` | `1.10.3` |
| `WINEHUA_DXVK_ROOT` | `…wine/dxvk/modern-2.6` | `…wine/dxvk/legacy` | `…wine/dxvk/legacy` | `…wine/dxvk/legacy` |
| `WINEHUA_DXVK_RELAXED_FEATURES` | `—` | `1` | `1` | `1` |
| `DXVK_WINEHUA_BATCH_MAPPED_FLUSH` | `—` | `1` | `1` | `1` |
| `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED` | `—` | `1` | `1` | `1` |
| `DXVK_WINEHUA_COMMAND_QUERY_RESET` | `—` | `1` | `1` | `1` |
| `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT` | `—` | `auto` | `auto` | `auto` |
| `WINEHUA_PRESENT_BACKEND` | `venus_broker_present` | `venus_broker_present` | `—` | `venus_broker_present` |
| `BOX64_DYNAREC_WEAKBARRIER` | `0` | `0` | `0` | `0` |
| `APPSPAWN_FD_wineserver_sock` | `—` | `—` | `24` | `—` |
| `WINESERVERSOCKET` | `—` | `—` | `29` | `—` |
| `MANGOHUD_CONFIG` | `—` | `—` | `legacy_layout=0,custom_text_center=Box64 arm64 v…` | `—` |
| `BOX64_SYSINFO_NCPU` | `—` | `—` | `12` | `—` |
| `WINEHUA_GUEST_GFX_MODE` | `mesa-virpipe` | `mesa-virpipe` | `mesa-virpipe` | `mesa-virpipe` |
| `LIBGL_ALWAYS_SOFTWARE` | `1` | `1` | `1` | `1` |
| `WINEHUA_WORKING_DIRECTORY` | `/data/storage/el2/base/files/.wine/drive_c/smoke…` | `/data/storage/el2/base/files/.wine/drive_c/smoke…` | `—` | `C:\smoke\x64` |
| `WINEHUA_DESKTOP` | `shell` | `shell` | `—` | `shell` |

## 5. 被排除的因子 (曾被怀疑, 均有反例)

- **DXVK legacy 档本身** ❌: D 用 legacy 1.10.3 帧率高.
- **legacy 兼容五件套** (BATCH_MAPPED_FLUSH / FLUSH_DYNAMIC_MAPPED / COMMAND_QUERY_RESET /
  EMULATE_RGBA8_SNORM_RT / RELAXED_FEATURES) ❌: D 全部带有仍高帧率.
- **启动路径** (应用库 vs 文件管理) ❌: B(应用库) 与 C(文件管理) 都低; A(应用库) 与 D(应用库) 都高.
- **MANGOHUD_CONFIG** (box64 hookMangoHud 注入) ❌: B 无 MANGOHUD 却低; D 无且高.
- **BOX64_SYSINFO_*** ❌: 同上反例.
- **VN_PERF 长短组合 / no_semaphore_feedback 差异** ❌: A(4项) 高; D(2项) 高; B/C(3项) 低 → 无一致关系.
- **WINEHUA_PRESENT_BACKEND / WORKING_DIRECTORY 等启动侧上下文** ❌: 与帧率无隔离关系.

## 6. 排查方法 (拿到一个慢场景快照后)

1. 取该进程快照 (发射时落盘 temp/proc_snapshot.log), 找到其 env.
2. 看 `WINEHUA_D3D_BACKEND`:
   - `vkd3d_limited_500k` → 进第 3 步 (混合路由);
   - `dxvk_*` (纯路由) → 帧率慢另有原因, 与本文档失配无关, 查 `VKR_/WINEHUA_VKR_/BOX64_/VN_` 性能开关与 per-frame 日志.
3. 看 `WINEHUA_DXVK_PROFILE`:
   - `legacy` → 命中已知失配 (机制组×1.10.3), 修复方向见 §8;
   - `modern-2.6` → 失配不成立; 若仍慢, 查同第 2 步分支.
4. 复核机制组键是否齐全 (§3.1 全部同现才算机制组在; 半套出现是其它注入差异).
5. 帧率数据佐证: DXVK_LOG_PATH 日志时间戳 / WINEHUA_DISPLAY_FPS_FILE 或 hilog `MW-TAKE` (90edaae)
   / 用户观测.

## 7. 机理 (推断, 有待进一步验证)

vkd3d 混合路由引入 venus 影子内存协作机制: guest DXVK 的 buffer 上传走 host 侧精确影子
(SHADOW_FROM_HOST=precise) + 强 ring barrier + direct fence wait + coherent map sync —
设计目标是给 venus 共享环一个 "host 可观测, guest 可直写" 的同步模型。DXVK 1.10.3 的经典
子路径 (频繁 map/unmap + 隐式同步) 与这套模型不匹配: 每帧资源更新被迫回退到 host 同步
/恢复路径 → 帧率腰斩。DXVK 2.6.2 的 ring/upload 语义是本项目按新机制适配的, 故匹配良好。
1.0.12 纯 DXVK 路由不注入机制组, 1.10.3 经典语义在经典 vtest socket 直传上运行 → 无冲突。

## 8. 修复候选 (按优先级)

1. **vkd3d 混合路由下, DXVK 档位联动**: 机制组在时默认/强制 `dxvk_modern_2_6`
   (产品默认既有的能力契约: 失败降回 legacy 前 **如 90edaae DXVK_MODERN_UPGRADE_READINESS
   的 DXVK_MODERN → DXVK_LEGACY → WineD3D 顺序**; 注意该降级本身会落到慢组合, 需一并评估);
2. **全局档位贯通引擎会话**: 文件管理路径目前按引擎基线 (legacy) 走, 应读同一全局
   `winehua.dxvk.preference` (设置页档位对文件管理路径生效 — 产品一致性);
3. **调研 1.10.3 × 机制组的降级策略**: 机制组某些键 (如 DIRECT_FENCE_WAIT /
   PERSISTENT_MAP_SYNC) 在 1.10.3 下是否能免除而不破坏正确性 — 需 venus 正确性回归 (关掉后
   先跑 D3D11 smoke 防止画面错乱);
4. 长期: 1.10.3 明确标记为 "旧设备能力/正确性回退档", 默认产品档 = 2.6.2。

## 9. env 注入源头 (排查时对照)

| 键组 | 注入位置 |
|---|---|
| WINEHUA_DXVK_/WINEHUA_VKD3D_/WINEDLL*/VN_PERF/VKR_WINEHUA_SHADOW_FROM_HOST/VKD3D_WINEHUA_FORCE... / WINEHUA_PERF_PROFILE | entry/src/main/cpp/wine_env.cpp `AppendD3dBackendEnv` (vkd3d_limited_500k 分支 / dxvk_* 分支) |
| WINEHUA_PRESENT_BACKEND / WORKING_DIRECTORY / DESKTOP / AUTOMATION | ArkTS 侧 runWineProgram (AppLibraryService/WineEnvService) |
| MANGOHUD_CONFIG / BOX64_SYSINFO_* | thirdparty/box64 core.c `hookMangoHud` / 系统信息缓存 |
| DXVK_WINEHUA_PRECISE_SHADOW / DXVK_LOG_PATH=C:\windows\temp | wine 侧默认 (会话链) |
| 引擎会话基线 (explorer 链, 文件管理继承) | 引擎启动时注入 (当前基线段 DXVK=legacy) |
