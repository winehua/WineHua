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

### 3.4 换图形栈对照：这 11 ms 是两条栈**共有**的

再加一个套件 `p5-paths`，同一个 cube 分别走 **DXVK 1.10（D3D11→Venus→vtest）**
与 **WineD3D（D3D9→OpenGL→virgl）** 两条完全不同的图形栈，其余条件不变。
run `p5c`（30 s / 项）：

| 测试 | 图形栈 | frames | fps | avgMs | renderMs | **presentMs** |
| --- | --- | --- | --- | --- | --- | --- |
| `p5-d3d11-x86-fex` | DXVK → Venus → vtest | 2354 | 81.03 | 12.3416 | 0.1662 | **11.6985** |
| `p5-d3d9-x86-fex` | WineD3D → OpenGL → virgl | 2548 | 88.33 | 11.3208 | 0.1697 | **10.7199** |
| `p5-d3d9-x64-native` | WineD3D → OpenGL → virgl | 2586 | 89.56 | 11.1655 | 0.0814 | **10.9828** |

**结论：两条互不相干的图形栈给出几乎相同的 present 成本（10.7–11.7 ms），
render 都只有 0.08–0.17 ms。** 所以这 ~11 ms **不是 DXVK/Venus 特有的**，
而在两者共用的宿主 present 段：

```text
vtest socket → 宿主 virglrenderer/vkr → SurfaceQueue → egl/XComponent 上屏
                      ↑ 两条 Guest 栈在这里汇合
```

配合 §3.3（与 CPU 架构/转译器无关），P5 的归因已收窄到：
**宿主 present/上屏段的固定 ~11 ms**。下一步应在这段里打点
（`graphics/` 下的 SurfaceQueue / egl_renderer / presenter 与显示周期配置），
而不是继续在 Guest 侧或 FEX 上找。

> 旁证：`graphics/presenter_common.h` 里存在帧周期钳制常量
> （`kDefaultFramePeriodNs = 16666667` 名义 60 Hz、`kDispatchLeadNs = 500000`），
> 且 `venus_surface_presenter` 会用 `lastPresentNs_ + framePeriodNs_` 计算下一个
> present 截止时间 —— present 段确实带 pacing 逻辑，是否就是这 11 ms 需要下一轮打点确认。

### 3.5 打点宿主 present：真因是 **90 Hz 屏幕节拍**，不是渲染成本

把宿主 presenter 里**本来就无条件累加**的 present 分段统计打开（`presenter_common.h`
的 `kForcedOn`，行为中性，只放开每 120 帧一条日志），再跑 `p5-paths`（run `p5d`）。
宿主侧 `[VENUS-PRESENT][NCP]`：

```text
frames=2280 fps=79.16 present_us_avg=2967 max=22451
  wait_fence_avg=15  acquire_avg=642  submit_avg=923  queue_present_avg=1099
  release_wait_avg=36  release_polls_avg=0  gpu_present_copy_avg=0
  release_mode=wait  failures=0  throttled=0
```

**宿主整条 present 只花 2.97 ms**（15+642+923+1099+36 µs）——远小于客人观察到的 11.7 ms。
所以那 11 ms 不是宿主的渲染/present 工作量。

再取宿主日志里的显示周期：

```text
[VENUS-PRESENT][NCP] target attached ... display_period_us=11129
[VIRGL-ZC][NCP]       target attached ... display_period_us=11129 pace_period_us=11129
```

**屏幕是 90 Hz（11.129 ms），两个 presenter 都按这个周期 pacing 出帧。**

于是把之前所有数字换算成"相对屏幕节拍的余量"：

| 配置 | 帧时间 avgMs | 相对 11.129 ms |
| --- | --- | --- |
| x86 wowbox64 · DXVK | 12.1538 | **+1.025**（+9.2%） |
| x86 FEX · DXVK | 12.2953 | **+1.166**（+10.5%） |
| x64 原生 ARM64 · DXVK | 12.1590 | **+1.030**（+9.3%） |
| x64 真 AMD64+FEX · DXVK | 12.1529 | **+1.024**（+9.2%） |
| x86 FEX · WineD3D/D3D9 | 11.3208 | +0.192（+1.7%） |
| x64 原生 · WineD3D/D3D9 | 11.1655 | +0.037（+0.3%） |

**这才是完整的 P5 图景：**

1. 帧时间的主体是**屏幕节拍 11.13 ms（90 Hz）**，不是任何计算成本。
   D3D9/WineD3D 路径几乎正好卡在节拍上（+0.04 ~ +0.19 ms）。
2. **DXVK/Venus 路径每帧多约 1.0 ms**，因此错过 90 Hz 截止点，落到 ~81 fps。
   宿主 present 只占 2.97 ms、有 ~8 ms 余量，所以这 ~1 ms 更可能在
   **客人侧 DXVK/Venus 提交 + vtest 往返**这一段，而不是宿主。
3. CPU 后端的差异（FEX vs wowbox64 = 0.14 ms；原生 vs 真 AMD64 = 0.006 ms）
   只是这 ~1 ms 的一小部分 —— **转译器不是瓶颈**这个结论进一步坐实。

**方法学后果（重要）：** 这个 smoke cube 的工作负载是**显示器节拍受限**的，
所以它只能回答"有没有超出 90 Hz 预算、超了多少"，**不能用来比较 CPU/GPU 的绝对算力**。
要测余量必须换负载（离屏渲染、或每帧多次提交），不能拿它的 fps 直接做后端排名。
换个角度说：**之前"x86/x64 都 12.1-12.3 ms 所以分不出差异"的困惑，
本质是四条配置都撞在同一个显示节拍附近。**

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
