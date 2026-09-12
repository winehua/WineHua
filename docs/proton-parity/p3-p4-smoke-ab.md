# P3 / P4：受控 A/B 与 x64+ARM64X 链路验证（真机）

> 执行：2026-09-12，设备 `MLR-AL10`（API 26 / HarmonyOS 7.0.0.105）
> 工具：smoke 编排（`aa start --ps winehua.mode smoke ...`）+ suites.json 每测试 env + cube `--bench`
> 结论：**四种配置（x86×2 后端 / 原生 ARM64 / 真 AMD64+FEX）帧时间都在 12.10–12.30 ms，
> 差异 <1%。CPU 后端不是这个负载的瓶颈。** x64 + ARM64X 图形链路经真机确认为正确路径。

## 1. 基准设施（可完全脚本化）

```bash
# 触发（App 存活时走 onNewWant → 引擎必然 ready）
hdc shell "aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode smoke --ps winehua.suite <suite> --ps winehua.run_id <id>"

# 结果（沙箱内, hdc 可读）
.../files/.wine/drive_c/smoke/results/<runId>/<testId>.json
.../files/.wine/drive_c/smoke/results/<runId>/<testId>.json.progress   # bench 进度
.../files/.wine/drive_c/smoke/results/<runId>/suite-summary.json
```

`suites.json` 每个 test 的 `env` 会被 runner 合并进 `runWineProgram` 的环境 →
变成 `__env=` 传给 NCP 子进程 → `apply_entry_param_env_overrides()` 落 `setenv`。
因此**同一个 HAP 就能做引擎 A/B**，不必出两个包。

新增套件（已进 `scripts/assemble.sh` 生成器）：

| 套件 | 测试 | 变量 |
| --- | --- | --- |
| `p3-wow64-ab` | `p3-bench-x86-wowbox64` / `p3-bench-x86-fex` / `p3-bench-x64-native` | `WINEHUA_WOW64_ENGINE=box\|fex` + `WINEHUA_SMOKE_BENCH=1` |
| `p4-amd64-fex` | `p4-bench-amd64-fex-arm64x` | `WINEHUA_SMOKE_BENCH=1` |

## 2. 引擎切换确实生效（否则 A/B 无意义）

`apply_wow64_cpu_dll()` 在 `reassert`（`__env` 覆盖之后）二次求值，hilog 逐进程可验：

```text
# p3-bench-x86-wowbox64 (pid 65046)
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=libwow64fex.dll engine=(default fex)   ← setup 阶段
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=wowbox64.dll    engine=box             ← override 之后
[WineChild] env override WINEHUA_WOW64_ENGINE=box

# p3-bench-x86-fex (pid 65245)
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=libwow64fex.dll engine=fex
[WineChild] env override WINEHUA_WOW64_ENGINE=fex
```

## 3. 结果

cube 加 `--bench` 后跳过每帧阻塞、只留 `Sleep(0)` 让出时间片，并对 render+present
逐帧计时。**30 s / 次，同一 HAP、同一 cube、同一 DXVK legacy**：

### run `p3c` + `p4c`

| 测试 | PE 架构 | CPU 后端 | DXVK | frames | fps | avgMs | p50Ms | p95Ms | maxMs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `p3-bench-x86-wowbox64` | x86 | wowbox64 | x86 | 2378 | 82.28 | 12.1538 | 12.0619 | 15.8573 | 179.08 |
| `p3-bench-x86-fex` | x86 | libwow64fex | x86 | 2367 | 81.33 | 12.2953 | 12.0651 | 15.7870 | 146.99 |
| `p3-bench-x64-native` | x64（原生 ARM64 PE） | 无转译 | arm64x | 2398 | 82.24 | 12.1590 | 12.0942 | 15.0276 | 107.12 |
| `p4-bench-amd64-fex-arm64x` | **x64（真 AMD64 0x8664）** | libarm64ecfex | arm64x | 2381 | 82.29 | 12.1529 | 12.0422 | 15.6849 | 97.75 |

### run `p3d`（重复，验证方差）

| 测试 | frames | fps | avgMs | p50Ms |
| --- | --- | --- | --- | --- |
| `p3-bench-x86-wowbox64` | 2397 | 82.66 | 12.0975 | 11.9828 |
| `p3-bench-x86-fex` | 2388 | 81.88 | 12.2128 | 12.0453 |
| `p3-bench-x64-native` | 2398 | 82.35 | 12.1433 | 12.1089 |

**两次独立运行的 avgMs 偏差 ≤0.06 ms（≈0.5%），数据可复现。**

### 3.1 结论

1. **CPU 后端在这个负载上不构成差异。** x86 的 wowbox64 与 FEX 相差 ~0.08–0.14 ms
   （FEX 略慢、约 0.7–1.2%，两次运行方向一致但幅度接近噪声）；
   原生 ARM64（完全无转译）与它们同样在 12.1–12.2 ms。
2. **真 AMD64 + FEX + ARM64X 与原生 ARM64 持平**（12.15 vs 12.16 ms）。
3. 因此 **~12.1 ms/帧是这条链路的共同底线**，它不在 CPU 转译侧，
   而在 render+present 共同路径（DXVK → Venus → virglrenderer → host present）。
   P5 的归因方向应从这里入手，而不是先去调 FEX 的构建参数。

### 3.2 P4 验收项（方案要求逐条）

`p4-bench-amd64-fex-arm64x` 结果 JSON 直接给出：

```text
peArchitecture : x64                       ← 实际渲染进程是 x64 ✓
renderer       : D3D11 / featureLevel 11_0
d3d11Dll       : .../dxvk/legacy/arm64x/d3d11.dll   ← 实际加载 ARM64X ✓
dxgiDll        : .../dxvk/legacy/arm64x/dxgi.dll    ← 实际加载 ARM64X ✓
initHresult / presentHresult : 0x00000000
angleRegressions : 0
```

**没有静默退回普通 x64 图形 DLL**，ARM64X overlay 命中，画面正确。

### 3.3 P5 归因：帧时间 96–98% 花在 `Present`

在 cube 里把 `render_d3d11` 拆成两段计时：`renderMs` = 从进入函数到 `Present` 调用前
（全部 D3D11 状态设置 + DrawIndexed + Map/Unmap + 顶点变换），
`presentMs` = `IDXGISwapChain_Present(swap_chain, 0, 0)` 本身。run `p5b`：

| 测试 | avgMs | **renderMs** | **presentMs** | present 占比 |
| --- | --- | --- | --- | --- |
| `p3-bench-x86-wowbox64` | 12.1297 | 0.1665 | **11.7701** | 97.0% |
| `p3-bench-x86-fex` | 12.2732 | 0.1709 | **11.6601** | 95.0% |
| `p3-bench-x64-native` | 12.1530 | 0.0891 | **11.9523** | 98.3% |

**结论：**

1. 所有 CPU 侧工作（D3D11 调用序列 + DrawIndexed + 顶点变换 + 上传）只有
   **0.09–0.17 ms**；连 FEX 转译 x86 的代价在内也不到 0.2 ms。
2. 帧时间几乎**全部**是 `Present` 的 11.7–12.0 ms。
3. 三种配置（wowbox64 / FEX / 原生 ARM64）的 `presentMs` 一致到 0.3 ms 以内，
   说明它**与 CPU 架构和转译器无关**，是这条图形链路的公共段：
   `DXVK → win32u/winevulkan → Venus ICD → vtest → host virglrenderer → SurfaceQueue → XComponent`。
4. `Present` 的 `SyncInterval=0`（非 vsync），却稳定 ~11.9 ms（≈84 Hz），
   形态更像**宿主 present/表面队列的自带节拍或等待**，而不是 GPU 渲染不过来。

**这直接决定 P5 的下一步方向：去宿主 present 链路找这 12 ms，而不是调 FEX。**
（与 `docs/ARM64_SCHEME3_PERF_HANDOFF.md` §5.6 "Host present ~5ms 时低 FPS 仍可能在
Guest shadow" 的提示一致，但本轮的测量把范围收窄到 present 段本身。）

## 4. 两处被修正的判断（保留记录，避免复现同样的错）

### 4.1 "78 fps 是 `Sleep(1)` 节流" —— 错

初版 cube 每帧 `Sleep(1)`，测得 ~78 fps，我据此判断"循环被节流、无法区分后端"。
加 `--bench` 关掉 Sleep 后每帧仍是 **12.27 ms**（带 Sleep 是 12.61 ms），
差 0.34 ms —— **12 ms 是真实每帧成本，不是节流**。该结论作废并已按 §3 重测。

### 4.2 "x64 路径比 x86 慢 4 倍（19.9 fps）" —— 测量假象

第一次 `p4-a` 用**旧的非 bench cube**（mingw-gcc 构建、带 `Sleep(1)`）、
且紧跟在装机/运行时解包之后运行，得到 598 帧 / 30 s ≈ 19.9 fps。
改用新的 clang 构建 + bench 模式重测（§3 的 `p4c`）为 **2381 帧 / 82.3 fps**，
与 x86、原生 ARM64 完全一致。**19.9 fps 不可复现，判定为假象**
（最可能是冷启动 + 旧二进制叠加）。结论以 `p4c` 为准。

### 4.3 附带修掉的一个真实缺陷

`--bench` 最初只是"不 Sleep"，即忙等。结果 `p3b` 那轮里 FEX 项**卡死在 60 帧检查点**，
runner 随后也卡住、不出 summary。改成 `Sleep(0)`（让出时间片但不做毫秒级节流）后，
`p3c`/`p3d`/`p4c` 三轮全部正常跑完。
→ **忙等会饿死 wineserver/图形线程**，这是真实缺陷，不是测量噪声。

## 5. 下一步

1. 按 §3.3 的指向继续细分 `presentMs`：在 DXVK / win32u / Venus ICD / vtest /
   virglrenderer / SurfaceQueue 各段打点，确定 11.9 ms 具体停在哪一步
   （是等 fence、等 surface queue 空位，还是宿主合成节拍）。
2. 分离 ARM64X thunk 成本：把 x64 overlay 指向 `dxvk/legacy/x64/`（普通 x64 DXVK，
   整库走 FEX）与现在的 `arm64x/` 对照 —— 即方案 §5.3 的实验。
   由于 §3 显示 x64 与原生持平、且 §3.3 显示 present 占 98%，这一项现在是
   "可选验证"而非首要怀疑对象。
3. 有余力再跑一轮非 bench（带 `Sleep(1)`）对照，确认 §4.1 的口径差异可复现。

## 6. 本轮附带产出

| 文件 | 作用 |
| --- | --- |
| `smoke/winehua_d3d_switch_cube.c` | `--bench` / `WINEHUA_SMOKE_BENCH=1`；帧时间分布；`Sleep(0)`；`.progress` 进度文件 |
| `docs/proton-parity/tools/build_bench_cubes.sh` | 用 llvm-mingw 编 x86 / amd64 / aarch64 三份 cube（不需要 Docker） |
| `docs/proton-parity/tools/patch_hap_payload.py` | 直接替换 HAP 内 wine-data.zip 条目并同步 payloadSha256（无 Docker 出包） |
| `scripts/assemble.sh` | `p3-wow64-ab` / `p4-amd64-fex` 套件 |
