# P5：合入判定与回退策略

> 依据：方案 §7 P5 的合入前要求
> 「正确性通过、启动/退出稳定、目标游戏性能有可解释结果、配置明确、
>   稳定后端可回退，且没有混入无法追踪的图形路径变化」
> 结论日期：2026-09-12

## 1. 逐条核对

| 合入要求 | 证据 | 判定 |
| --- | --- | --- |
| 正确性通过 | `p3c/p3d/p4c/p5b/p5c/p5d/p5e` 共 7 轮 smoke 全 PASS；`angleRegressions=0`；D3D11 `featureLevel=11_0`、`init/present HResult=0`；x64 用例实际加载 `dxvk/legacy/arm64x/{d3d11,dxgi}.dll`（未退回 x64 图形 DLL） | ✅ |
| 启动/退出稳定 | 7 次 `bm uninstall` + `bm install` + 冷启动全流程成功；每次运行后有 19–30 个 wine 子进程存活，`hilog` 无 `CRASH/SIGSEGV/SIGABRT/fatal` | ✅ |
| 配置明确 | `FEX_Config.json`（Proton 参考值）随包部署到 `share/fex-emu/Config.json`，由 `FEX_APP_CONFIG_LOCATION` 定位；真机 SHM 统计创建成功证明配置**确实被读取** | ✅ |
| 稳定后端可回退 | `WINEHUA_WOW64_ENGINE=box` 真机验证可切回 wowbox64；完整 x86_64 Wine + Box64 方案②链路未被本分支触碰 | ✅ |
| 未混入无法追踪的图形路径变化 | 本分支对图形的改动只有：`assemble.sh` 归位两个 FEX UnixLib `.so`、smoke cube 增加 `--bench`。presenter 上做过一次临时诊断开关，**已还原**（`kForcedOn = false`）。DXVK / vkd3d / Mesa / virglrenderer / Host present 策略均未改 | ✅ |
| **目标游戏性能有可解释结果** | **不满足** —— 见 §2 | ❌ |

## 2. 为什么"目标游戏性能"这一条不满足

本分支的性能测量全部基于内置 smoke cube。打到宿主日志后确认
（`p3-p4-smoke-ab.md` §3.5）：

```text
display_period_us = 11129   （屏幕 90 Hz）
两个 presenter 都按 11.129 ms pacing 出帧
```

**这个负载是显示节拍受限的**：帧时间主体就是屏幕节拍，四条 CPU 配置
（wowbox64 / FEX x86 / 原生 ARM64 / 真 AMD64+FEX）全部落在 12.05–12.33 ms，
差异 <1%；D3D9/WineD3D 路径甚至正好卡在节拍上（+0.04–0.19 ms）。

它能回答的是「有没有超出 90 Hz 预算、超了多少」，**不能**回答「这个后端在真实
游戏里快多少」。要给出目标游戏的可解释性能结论，必须换一个不吃显示节拍的负载
（离屏渲染、或每帧多次提交），或者直接在目标游戏上测。

### 已经可以下的性能结论（有证据）

| 结论 | 证据 |
| --- | --- |
| CPU 转译器不是瓶颈 | 原生 ARM64 12.159 ms vs 真 AMD64+FEX 12.153 ms（差 0.006 ms）；x86 FEX vs wowbox64 差 0.14 ms |
| 转译工作量极小 | cube 的 `renderMs`（全部 D3D11 调用 + Draw + 变换 + 上传）只有 0.09–0.17 ms |
| 剩下的是公共图形成本 | 宿主 present 只有 2.97 ms（有余量）；DXVK/Venus 比 D3D9/WineD3D 每帧多约 1.0 ms，因而错过 90 Hz 落到 ~81 fps |
| 构建参数对齐不提速 | Release + profiler + TUNE_CPU=none 对齐前后帧时间同噪声范围（`fex-build-parity.md`） |
| render 侧余量极大 | 每帧绘制量 ×4（K=1→K=4）后 `renderMs` 只动 0.01–0.02 ms，帧时间不动（`p3-p4-smoke-ab.md` §3.6） |
| 离屏方案在本运行时不可用 | 不 Present 时无消费者，`Flush` 会灌满 vtest ring，第 60 帧卡死（同样记录在 §3.6） |

## 3. 判定

**P5 判定：ARM Runtime 达到"功能对齐、可回退"的状态，但不建议现在切换产品默认后端。**

理由：

1. 功能面已经闭合：UnixLib 加载、SHM 统计可读、x64+ARM64X 图形链路、FEX 构建参数对齐，
   都有真机证据；正确性、稳定性、配置、回退路径四条的验证也都通过。
2. 唯一缺的是"目标游戏性能有可解释结果"。在当前测量手段下这一条**无法**被满足，
   而不是"结果不好"——所以不能拿现有数据宣称对齐完成或未完成。
3. 现有数据显示的 ~1 ms DXVK/Venus 超预算问题**对所有配置一视同仁**，
   不是 Proton 对齐引入的；把它记成独立课题更合适。

## 4. 回退策略（都已验证可执行）

| 层级 | 回退动作 | 影响面 |
| --- | --- | --- |
| 整个实验 | 丢弃 `feature/proton-arm64-parity` 分支（产品 `feature/arm64-heaven-port` 从未被修改） | 零 |
| 运行时后端 | `WINEHUA_WOW64_ENGINE=box` 把 32 位切回 wowbox64（真机已验证） | 单个启动会话 |
| FEX 构建 | `FEX_BUILD_TYPE=RelWithDebInfo FEX_PROFILER=False` 回到旧基线参数 | 下次出包 |
| FEX 补丁 | `scripts/build_fex.sh` 的 `FEX_PATCHES` 去掉 `fex-unixlib-backport.patch` | 下次出包 |
| 生效配置 | 删除 `FEX_Config.json` → FEX 回落源码默认值（`maxInst=5000` 等） | 下次出包 |
| 探针 | `FEX_UNIXLIB_PROBE` 默认关闭；`presenter_common.h` 的 `kForcedOn` 已为 `false` | 已恢复 |

## 5. 要做到"目标游戏性能有可解释结果"，下一步需要什么

1. **一个不吃显示节拍的负载**：cube 加离屏渲染模式（渲染到 RTV、不 Present），
   或每帧多次提交；用它做后端排名。当前 `--bench` 已经解决了"被 `Sleep(1)` 掩盖"
   的问题，但还没解决"被屏幕节拍掩盖"。
2. 在 1) 的基础上重跑 x86 两后端 + x64 两形态，给出**余量倍数**而不是 fps。
3. 有条件时在目标游戏（Unity x64 / Heaven x86）上做同样口径的对照。

## 6. 分支交付物清单

P0/P1/§12 的文档见 `README.md`；P2–P5 的证据文档：
`p2-release-probe.md`、`p2-unixlib-build.md`、`p2-device-validation.md`、
`p2-effective-config.md`、`p3-p4-smoke-ab.md`、`fex-build-parity.md`、本文。

实验工具（不进产品构建路径）：
`tools/build_bench_cubes.sh`、`tools/patch_hap_payload.py`、`tools/stage_runtime_fex.py`、
`tools/repack_hap_unixlib.py`；探针补丁 `scripts/patches/fex-unixlib-probe.patch`。
