# WineHua automation（WSL 回归测试）

对长期维护项目做回归测试的套件运行器。纯 WSL 环境，Pad 直连（或任何 `hdc list targets` 可见的设备）。

## 文件

| 文件 | 用途 |
|------|------|
| `run_regression.py` | 回归套件运行器：Docker HAP 构建 → 校验 HAP payload → `hdc install` → smoke 模式启动 → 固定帧截图校验 → D3D11 覆盖率 / capability 矩阵 → 归档结果 |
| `validate_frame.py` | PIL/numpy 像素校验（`rgba-quadrants-v1-rotations`、`d3d11-cube-color-depth-v1`），`run_regression.py` 直接 import |

> 2026-08-01 整理说明：Phase 2 DXVK 调查期的一次性工具（Heaven A/B 启动脚本、帧序测量、UBO/present 身份链 trace 分析脚本）已删除，可从 git 历史找回。调查期 suite/profile 也不再是合法参数。

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

# 能力矩阵（Host Vulkan vs Venus 能力对比 + hash）
python3 automation/run_regression.py --suite capabilities

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
| `--suite` | `core` | `core audio opengl d3d8 d3d9 wine-vulkan wine-vulkan-present venus venus-sampled venus-depth-cube-array-2d-golden dxvk dxvk-long dxvk-dynamic gpu-diagnostics dxvk26-requirements dxvk-modern-baseline dxvk-modern-long capabilities all long` |
| `--prefix` | `reuse` | `reuse`（复用 prefix）/ `clean`（App UID 下重建 prefix） |
| `--perf-profile` | `shadow-precise-dirty-ring-inline-upload-coverage-sort` | 产品 profile；诊断可选 `shadow-precise-dirty-ring-frame-timeline` |
| `--runs` | 1 | 重复运行次数 |
| `--long-seconds` | 3600 | `dxvk-long` 套件的墙钟目标 |
| `--gate` | off | 入口门禁（3×reuse core + 1×clean core） |
| `--skip-build` | off | 跳过本机 make，直接用现有 HAP |
| `--device-id` | 自动 | hdc target |
| `--archive-root` | 自动 | 结果归档根 |
| `--timeout-minutes` | 15 | 单 run 超时（dxvk-long 自动放宽） |

## 套件说明

- **core**：OpenGL smoke（x86+x64）固定帧 + 四象限视觉校验——最小回归门禁
- **opengl / audio / d3d8 / d3d9**：对应 guest smoke 套件
- **wine-vulkan / wine-vulkan-present**：Wine Vulkan + present 链路
- **venus / venus-sampled / venus-depth-cube-array-2d-golden**：Venus 后端能力/采样/深度资格
- **dxvk**：D3D11 完整功能矩阵（feature level、texture/descriptor/subresource/3D/UAV/BC、固定帧 cube、零 CPU readback/upload、无 WineD3D fallback）
- **dxvk-long**：长时间稳定性（默认 1 小时）
- **dxvk-dynamic**：dynamic constant buffer 专项
- **gpu-diagnostics**：报告 Guest Vulkan、DXVK DLL 实际加载路径和 D3D11 device 状态
- **dxvk26-requirements**：DXVK 2.6.2 所需的 Guest/Wine Vulkan 1.3 transport 资格 probe
- **dxvk-modern-baseline / dxvk-modern-long**：能力门控的 DXVK 2.6.2 x86/x64 baseline、Cube 与长稳回归；Vulkan 1.2 设备应报告 `UNSUPPORTED`，不会伪造能力
- **capabilities**：Host Vulkan vs Venus 能力规范化 + hash + 差异矩阵
- **all / long**：组合套件

## 结果归档

每次运行生成 `regression-YYYYMMDD-HHmmss/`：

```
build.log / install.log / artifact.json     # 构建产物身份（HAP hash、submodule commits、架构）
automation-summary.json                     # 汇总：runs + status + capability hash
<run-id>/suite-summary.json                 # guest 侧套件结果
<run-id>/host-summary.json                  # host 侧判定（app + visual + coverage）
<run-id>/opengl-x64-visual.json             # 固定帧校验 JSON（validator + 质心/象限）
<run-id>/hilog.txt / wine-stderr.log        # 设备日志
<run-id>/device-results/                    # guest 结果原样拷贝
capability-matrix.json                      # capabilities 套件产物
```

退出码：全部 PASS 为 0，任一 FAIL/基础设施错误为 1。
