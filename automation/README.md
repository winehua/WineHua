# WineHua automation（WSL 回归测试）

对长期维护项目做回归测试的套件运行器。纯 WSL 环境，Pad 直连（或任何 `hdc list targets` 可见的设备）。

## 文件

| 文件 | 用途 |
|------|------|
| `run_regression.py` | 回归套件运行器：Docker HAP 构建 → 校验 HAP payload → `hdc install` → smoke 模式启动（Want 5 键协议）→ 固定帧截图校验 → D3D11 覆盖率判定 → 归档结果 |
| `validate_frame.py` | PIL/numpy 像素校验（`rgba-quadrants-v1-rotations`、`d3d11-cube-color-depth-v1`），`run_regression.py` 直接 import |

> 2026-08-31 smoke 设施重建设计（docs/SMOKE_REBUILD_20260831.md）后：capability 矩阵
> （host-vulkan/venus）、VKD3D Gate-A（vkd3d-capability）套件随 native 探针一并删除，
> 不再作为合法参数；dxvk-modern-baseline/long、d3d12（1000 帧）保留为 PE 套件。

## 前置条件

- **python3 + numpy + pillow**：`pip3 install numpy pillow`
- **hdc**：`WINEHUA_HDC` 环境变量指向 Windows HDC 或已加入 PATH
- **Docker**：可访问 `winehua-master-ext4` 容器；构建始终在容器的 `/data/src/winehua` 中完成
- **真机**：Pad USB/IP 直连（`hdc list targets` 可见）

## 用法

```bash
# 入口门禁：3 次 reuse-prefix core + 1 次 clean-prefix core
python3 automation/run_regression.py --gate

# 完整 D3D11 回归套件（x86 + x64，产品 profile）
python3 automation/run_regression.py --suite dxvk --prefix reuse

# D3D12（VKD3D-Proton limited-500k，1000 帧物理 present）
python3 automation/run_regression.py --suite d3d12 --prefix clean

# 跳过构建，只跑已产出的 HAP
python3 automation/run_regression.py --suite core --skip-build
```

脚本内不写死任何环境内容：

| 内容 | 解析方式 |
|------|---------|
| 仓库根 | 从 `run_regression.py` 所在目录推导 |
| hdc 路径 | `WINEHUA_HDC` env → PATH |
| 归档目录 | `--archive-root` → `WINEHUA_ARCHIVE_ROOT` env → `<repo>/build/automation-logs` |
| 设备 | `--device-id` → `hdc list targets` 自动选择（优先物理目标） |

## 参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--suite` | `core` | `core opengl audio d3d8 d3d9 wine-vulkan wine-vulkan-present dxvk dxvk-long dxvk-dynamic dxvk-modern-baseline dxvk-modern-long gpu-diagnostics dxvk26-requirements d3d12 all long` |
| `--prefix` | `reuse` | `reuse`（复用 .wine）/ `clean`（设备端重置编排：停→清 C 盘→自动重启） |
| `--runs` | 1 | 重复运行次数 |
| `--long-seconds` | 3600 | `dxvk-long` / `dxvk-modern-long` 套件的墙钟目标 |
| `--gate` | off | 入口门禁（3×reuse core + 1×clean core） |
| `--skip-build` | off | 跳过本机 make，直接用现有 HAP |
| `--device-id` | 自动 | hdc target |
| `--archive-root` | 自动 | 结果归档根 |
| `--timeout-minutes` | 15 | 单 run 超时（dxvk-long 自动放宽） |

Perf profile 不再可配：smoke 会话固定产品默认档（回归 = 出厂基线），与
`winehua.d3d_backend` 一并传给设备端的套件定义（suites.json）决定测试运行时后端。

## 设备端协议（Want，5 键）

`run_regression.py` 通过 `aa start` 携带（其余键无作用）：

| 键 | 值 | 说明 |
|------|------|------|
| `winehua.mode` | `smoke` | 进入自动化会话 |
| `winehua.suite` | 上表 suite 名 | 读取设备端 `C:\smoke\suites.json` 的套件定义 |
| `winehua.run_id` | ASCII | 结果目录 `C:\smoke\results\<run-id>\` |
| `winehua.prefix` | `reuse`/`clean` | clean → 设备端 `resetSmokePrefix` 编排（停→清 C 盘→重启） |
| `winehua.long_seconds` | 3600 | dxvk-long 系列墙钟目标 |

设备端编排：EntryAbility 解析 → AppStorage → WineEnvService 消费（引擎就绪后触发
SmokeRunner）→ SmokeRunner 逐测试 `runWineProgram`（统一 smoke argv 协议，见
`thirdparty/wine/programs/winehua_smoke_protocol.h`）→ 轮询结果 JSON → 汇总
`C:\smoke\results\<run-id>\suite-summary.json`。手动入口：App 侧边栏「Smoke 回归」按钮
（同链，suite=core）。

## 套件说明

- **core / opengl**：OpenGL smoke（x86+x64）固定帧 + 四象限视觉校验——最小回归门禁
- **audio / d3d8 / d3d9**：对应 guest smoke 套件（d3d9 = switch cube 视觉）
- **wine-vulkan / wine-vulkan-present**：Wine Vulkan offscreen 探针 + present 链路
- **dxvk**：D3D11 完整功能矩阵（feature level、texture/descriptor/subresource/3D/UAV/BC、
  固定帧 cube、零 CPU readback/upload、无 WineD3D fallback）
- **dxvk-long / dxvk-modern-long**：长时间稳定性（默认 1 小时，`--long-seconds` 可调）
- **dxvk-dynamic**：dynamic constant buffer 专项
- **dxvk-modern-baseline**：DXVK 2.6.2 x86/x64 baseline + Cube 回归；Vulkan 1.2 设备应报告
  `UNSUPPORTED`，不会伪造能力
- **gpu-diagnostics**：报告 Guest Vulkan、DXVK DLL 实际加载路径和 D3D11 device 状态
- **dxvk26-requirements**：DXVK 2.6.2 所需的 Guest/Wine Vulkan 1.3 transport 资格 probe
- **d3d12**：VKD3D-Proton limited-500k 1000 帧图形 smoke（含 checkpoint 进度）
- **all / long**：组合套件

> 二期预告：venus 系列（guest ELF 通道）、capabilities/host-vulkan（已删 native 探针）
> 如需恢复，另行评估；win32_driver game 点击驱动同理。

## 结果归档

每次运行生成 `regression-YYYYMMDD-HHmmss/`：

```
build.log / install.log / artifact.json     # 构建产物身份（HAP hash、submodule commits、架构）
automation-summary.json                     # 汇总：runs + status
<run-id>/suite-summary.json                 # guest 侧套件汇总（设备端 SmokeRunner 生成）
<run-id>/host-summary.json                  # host 侧判定（app + visual + coverage）
<run-id>/opengl-x64-visual.json             # 固定帧校验 JSON（validator + 质心/象限）
<run-id>/hilog.txt / wine-stderr.log        # 设备日志
<run-id>/device-results/                    # guest 结果原样拷贝
```

退出码：全部 PASS 为 0，任一 FAIL/基础设施错误为 1。
