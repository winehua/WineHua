# P3 / P4：受控 A/B 与 x64+ARM64X 链路验证（真机）

> 执行：2026-09-12，设备 `MLR-AL10`（API 26 / HarmonyOS 7.0.0.105）
> 工具：smoke 编排（`aa start --ps winehua.mode smoke ...`）+ `suites.json` 每测试 env
> 结论：**x64(FEX) 路径显著低于平台上限，是当前最明确的瓶颈信号**；x86 两个后端都顶到循环上限，本基准无法区分。

## 1. 基准设施

WineHua 自带 smoke 编排，可以完全脚本化，不需要手点 UI：

```bash
# 触发（App 存活时走 onNewWant → 引擎必然 ready）
hdc shell "aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode smoke --ps winehua.suite <suite> --ps winehua.run_id <id>"

# 结果（沙箱内, hdc 可读）
.../files/.wine/drive_c/smoke/results/<runId>/<testId>.json
.../files/.wine/drive_c/smoke/results/<runId>/suite-summary.json
```

`suites.json` 的每个 test 支持 `env`，会被 runner 合并进 `runWineProgram` 的环境
→ 变成 `__env=` 传给 NCP 子进程 → `apply_entry_param_env_overrides()` 落 `setenv`。
因此**同一个 HAP 就能做引擎 A/B**，不必出两个包。

新增套件（已进 `scripts/assemble.sh` 生成器）：

```json
"p3-wow64-ab": { "tests": [
  {"testId": "p3-cube-x86-wowbox64", "exe": "x86/winehua_d3d_switch_cube.exe",
   "env": {"WINEHUA_WOW64_ENGINE": "box"}, "d3dBackend": "dxvk_legacy", "seconds": 30},
  {"testId": "p3-cube-x86-fex", "exe": "x86/winehua_d3d_switch_cube.exe",
   "env": {"WINEHUA_WOW64_ENGINE": "fex"}, "d3dBackend": "dxvk_legacy", "seconds": 30}
]},
"p4-amd64-fex": { "tests": [
  {"testId": "p4-cube-amd64-fex", "exe": "amd64/winehua_d3d_switch_cube.exe",
   "env": {}, "d3dBackend": "dxvk_legacy", "seconds": 30}
]}
```

## 2. 引擎切换确实生效（否则 A/B 无意义）

用 `apply_wow64_cpu_dll()` 在 `reassert`（`__env` 覆盖之后）二次求值，hilog 逐进程可验：

```text
# p3-cube-x86-wowbox64 (pid 65046)
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=libwow64fex.dll engine=(default fex)   ← setup 阶段
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=wowbox64.dll    engine=box             ← override 之后
[WineChild] env override WINEHUA_WOW64_ENGINE=box

# p3-cube-x86-fex (pid 65245)
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=libwow64fex.dll engine=fex
[WineChild] env override WINEHUA_WOW64_ENGINE=fex
```

## 3. 结果（同一 HAP、同一 cube、同一 DXVK legacy、同一时长窗口）

| 运行 | PE 架构 | CPU 后端 | 时长 | frames | fps | 正确性 |
| --- | --- | --- | --- | --- | --- | --- |
| `p3-cube-x86-wowbox64` | x86 | wowbox64 | 30 s | 2360 | 78.7 | PASS，angleRegressions=0 |
| `p3-cube-x86-fex` | x86 | libwow64fex | 30 s | 2379 | 79.3 | PASS，angleRegressions=0 |
| `dxvk-cube-x64` | x64（方案③ `x64/` = 原生 ARM64 PE） | 无转译 | 8 s | 620 | 77.5 | PASS |
| `p4-cube-amd64-fex` | **x64（真 AMD64 0x8664）** | libarm64ecfex | 30 s | 598 | **19.9** | PASS，angleRegressions=0 |

### 3.1 x64 + ARM64X 图形链路的验收项（方案 P4）

`p4-cube-amd64-fex` 结果 JSON 直接给出：

```text
peArchitecture : x64                      ← 实际渲染进程是 x64 ✓
renderer       : D3D11 / featureLevel 11_0
d3d11Dll       : .../dxvk/legacy/arm64x/d3d11.dll   ← 实际加载 ARM64X ✓
dxgiDll        : .../dxvk/legacy/arm64x/dxgi.dll    ← 实际加载 ARM64X ✓
initHresult / presentHresult : 0x00000000
angleRegressions : 0
```

即：**没有静默退回普通 x64 图形 DLL**，ARM64X overlay 命中，画面正确。

### 3.2 重要发现：x64 路径是当前最明确的瓶颈

原生 ARM64、x86+wowbox64、x86+FEX 三者都在 **77.5–79.3 fps**，而同一 cube 的
真 AMD64 + FEX 只有 **19.9 fps**（2.5 s/帧 → 约 50 ms/帧）。也就是说：

1. ≈78 fps 是这条渲染循环的**上限**（见 §4 的方法学限制），前三者都顶到了上限；
2. x64 路径**顶不到上限**，差 4 倍 —— 这个差距只可能来自 x64 侧的执行成本。

可能来源（需要 P5 拆分，不能现在就归因）：

- FEX 翻译 x64 游戏代码的开销；
- 每次 D3D11 调用跨 ARM64X 边界的 thunk 成本（方案 §5.3 的假说）；
- 两者叠加。

方案里"x64 + ARM64X 应该更快"的预期，在本设备上**没有成立**，必须先解释这 4 倍。

## 4. 方法学限制（决定了这些数字能/不能说明什么）

`smoke/winehua_d3d_switch_cube.c`：

```c
/* D3D11 swap chain */  IDXGISwapChain_Present(s->swap_chain, 0, 0);   /* SyncInterval=0, 不限帧 */
/* render loop 末尾 */  Sleep(1);                                      /* 每帧固定睡眠 */
```

- `Present` 不限帧，但循环里每帧 `Sleep(1)`；实测 ≈12.7 ms/帧，说明 Sleep(1) 在
  OHOS 上的实际粒度就是十几毫秒。**这个 cube 因此被循环节流到 ≈78 fps。**
- 所以 **P3 的 x86 A/B 只能证明"两个后端都顶到上限、没有明显退化"，
  不能据此判断 FEX 与 wowbox64 的 CPU 成本差**。要用它做 CPU 归因，
  必须先去掉这个节流。
- x64 的 19.9 fps 低于上限，因此**不受此限制影响**，是有效信号。

## 5. bench 模式：修正 §4 的判断（重要）

给 cube 加了 `--bench` / `WINEHUA_SMOKE_BENCH=1`：跳过每帧 `Sleep(1)`，
并按帧时间分布输出 `avg/p50/p95/min/max`。

第一次实测（`run p3b`，30 s）：

| 测试 | 状态 | frames | avgMs | p50Ms | p95Ms | maxMs | fps |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `p3-bench-x86-wowbox64` | 完成 | 2378 | **12.2741** | 12.0886 | 15.6588 | 201.0682 | 81.5 |
| `p3-bench-x86-fex` | **停在 60 帧检查点，未产出终值** | (60) | (15.97) | (12.26) | (18.46) | (131.55) | — |
| `p3-bench-x64-native` | 未开始（runner 卡在前一项） | — | — | — | — | — | — |

**关键修正：§4 里"≈78 fps 是 Sleep(1) 节流上限"的说法不成立。**
关掉 Sleep 之后（bench 模式）每帧仍是 **12.27 ms**，而带 Sleep 的普通模式是
12.61 ms —— 差 0.34 ms，说明 `Sleep(1)` 在这个平台上只花约 0.3 ms，
**12 毫秒/帧是真实的每帧成本**，不是循环节流。

于是：

- 之前"三个后端都顶到 78fps 上限所以无法区分"的结论作废；
  这个 cube **是有区分度的**，x86 每帧 ~12.2 ms 是可以拿来做 A/B 的真实工作量。
- 但 bench 模式下 FEX 那一项**没有跑完**（停在 60 帧检查点后再无输出，
  runner 随后卡住，`suite-summary.json` 未生成）。这本身需要排查：
  去掉 Sleep 的紧凑循环 + FEX 转译的组合可能有问题（也可能是 runner 的
  完成判定问题——它在看到 60 帧检查点后就认为该项结束）。

## 6. 下一步

1. 修 bench 模式的完成判定 / 排查 FEX 项卡住：先确认 60 帧检查点是否让 runner
   提前放行（对比：非 bench 的 p3/p4 也是先写检查点，但最终都补上了终值）。
   若确认是 FEX 紧凑循环本身的问题，那是比帧率更值得先解决的缺陷。
2. 分离 ARM64X thunk 成本：把 x64 overlay 指向 `dxvk/legacy/x64/`（普通 x64 DXVK，
   整库走 FEX）与现在的 `arm64x/` 对照 —— 即方案 §5.3 的实验。
3. 有了 (1)(2) 再做正式的多次重复测量（交错顺序、记录温度/缓存状态），
   最后谈产品合入判定与回退策略。

## 7. 本轮附带产出

| 文件 | 作用 |
| --- | --- |
| `smoke/winehua_d3d_switch_cube.c` | 新增 `--bench` / `WINEHUA_SMOKE_BENCH=1`，输出帧时间分布 |
| `docs/proton-parity/tools/build_bench_cubes.sh` | 用 llvm-mingw 编 x86 / amd64 / aarch64 三份 cube（不需要 Docker） |
| `docs/proton-parity/tools/patch_hap_payload.py` | 直接替换 HAP 内 wine-data.zip 条目并同步 payloadSha256（无 Docker 出包） |
| `scripts/assemble.sh` | `p3-wow64-ab` / `p4-amd64-fex` 套件改用 bench 模式 |
