# 自动化回归测试方案 v2（替代现有 smoke 设施）

> 2026-09-16。目标：一套可积累用例、可回归、可跨环境运行的自动化测试设施，
> **替代**现有 smoke 设施（`automation/run_regression.py` + 设备端 `ets/smoke/` +
> HAP 内载荷）。只保留现有设施的骨架优点，重做用例组织、载荷通道与 host 工具链。

## 0. 需求与设计对照

| 要求 | 落点 |
|---|---|
| ① 少侵入：测试代码不干扰产品逻辑 | 载荷移出产品包（§4）；产品代码保持 7 处标记钩子（§6）；正常会话零行为变化 |
| ② 覆盖 + 稳定复现 + 跨环境 | 用例自包含目录（§3）；环境依赖收敛为 python3+hdc+mingw（§7）；判定与执行分离、可重跑（§8） |
| ③ 灵活：调试难复现场景/参数组合 | job 协议支持选测/参数覆盖/内联临时用例（§5）；矩阵展开（§3.3） |

## 1. 实测环境事实（2026-09-16，决定设计前提）

| 事实 | 证据 | 对设计的影响 |
|---|---|---|
| Linux hdc 直连设备可用 | `hdc list targets` 见 `192.168.1.5:44959` / `192.168.1.2:10178`；`aa start --ps` 传参已有先例 | host 工具不再需要 Windows PowerShell / Windows HDC |
| mingw 交叉编译器可用 | `/usr/bin/x86_64-w64-mingw32-gcc`（GCC 13）、`i686-w64-mingw32-gcc` | payload 本地构建，不需要 Docker |
| **无 Docker、无 PowerShell** | `which docker` / `which powershell.exe` 均空 | 现有 `invoke_build()`（`docker exec winehua-master-ext4`）在开发机跑不了 → 必须去掉 |
| hdc 可写 App 沙箱 | `hdc file send -b app.hackeris.winehua <local> /data/storage/el2/base/files/<name>` 实测成功，落点属主 `20020229`（app UID），App 可读 | 载荷不再需要打进 HAP（§4） |
| `-b` 的路径规则 | remote 必须写**沙箱视角**（`/data/storage/el2/base/files/...`）；写真实路径 `/data/app/el2/100/base/<bundle>/files/...` 报 no such file or directory；不用 `-b` 则 permission denied | host 工具按此规则封装，见 §4 |
| `hdc rport` 可用 | `hdc rport tcp:8848 tcp:8848` → `Forwardport result:OK` | 备选通道（HTTP 拉取），一期不做（§4.4） |
| 设备侧目录推送递归 | `hdc file send -b <bundle> <dir> <dst>` 整树送达（含子目录） | payload 可直接推目录树，不必打包 zip |

## 2. 现有设施：优点与缺点

### 2.1 吸收（保留骨架）

1. **测试定义数据化**：`suites.json` 声明 testId/exe/env/argv/seconds/timeoutMs，
   改套件不改 ArkTS（一期重建的核心成果）。
2. **结果 JSON 协议**：测试程序经 `winehua_smoke_protocol.h` 写
   `results/<run-id>/<test-id>.json`（status/stage/message/metrics），Runner 汇总
   `suite-summary.json`。协议稳定、与测试实现解耦。
3. **判定在 host**：设备端只负责"跑 + 收数据"，视觉/覆盖率判定在 host 侧，
   判定规则可迭代而不必重刷设备。
4. **物理隔离**：设备端 smoke 代码集中在 `entry/src/main/ets/smoke/`（整目录可删），
   产品文件只留 `[[SMOKE]]` 标记行。
5. **瘦解释器**：`SmokeRunner` 只搬运 env（不懂 env），产品语义由 native 管线收口；
   诊断 env 随载荷版本化在 suites.json。
6. **隔离 prefix + clean 编排**：`--prefix clean` 的 停→清→重启 编排保证干净基线。
7. **固定帧视觉校验**：`validate_frame.py` 的像素校验是"结果可复现"的关键手段。
8. **run-id 结果目录 + 归档结构**：结果与运行一一对应，可回溯。

### 2.2 改善（重做）

| # | 缺点 | 后果 | 新方案 |
|---|---|---|---|
| 1 | host 脚本依赖 Docker + Windows PowerShell | 只有作者机器能跑，"跨开发者环境稳定"不成立 | host 工具只用 python3 + hdc + mingw（§7） |
| 2 | 载荷（4.7 MB）打进 HAP（wine-data.zip 293 MB） | 改一个测试要重打 zip + 重装 HAP；产品包含测试资产 | 载荷移出产品包，`-b` 推送（§4） |
| 3 | Want 5 键协议（只能整套跑） | 不能跑单测、不能传参数组合、不能跑临时用例 | job 协议：选测 / 参数覆盖 / 内联用例（§5） |
| 4 | 新增用例要改 4 处（C 源、assemble 编译段、assemble suites 段、run_regression 白名单） | 用例积累摩擦大 | 用例 = 一个目录，构建脚本扫描生成（§3） |
| 5 | 判定逻辑绑死在 run_regression.py | 改判定要重跑设备；历史归档无法重判 | 判定器独立 + `check <run-dir>` 可重跑（§8） |
| 6 | SUITES 白名单双份维护（脚本 tuple vs suites.json keys） | 手工同步，易漂移 | 单一来源 = 用例目录（§3） |

## 3. 用例层

### 3.1 布局

```
smoke/
  tests/
    opengl-smoke/
      test.json                 # 用例定义（下节）
      opengl_smoke.c            # 源码（可以有多个，见 sources）
    dns-probe/
      test.json
      dns_probe.c
    d3d-switch-cube/
      test.json
      winehua_d3d_switch_cube.c
    ...
  include/                      # 跨用例共享头（如 vkd3d_capability_audit.h）
  assets/                       # 跨用例共享资产（venus_*.spv 等）
  suites/
    core.json                   # {"tests": ["opengl-smoke", "dns-probe"]}
    dxvk.json
    ...
```

一个用例一个目录：**新增用例 = 新建目录**，不改任何脚本。

### 3.2 test.json

```json
{
  "id": "dns-probe",
  "title": "wine dnsapi 解析（含 NXDOMAIN 反例）",
  "sources": ["dns_probe.c"],
  "arch": ["x64", "x86"],
  "build": { "libs": ["-lws2_32"] },
  "seconds": 6,
  "timeoutMs": 120000,
  "env": {},
  "argv": [],
  "checks": ["result-json"],
  "backend": { "d3d": "wined3d" }
}
```

字段语义：

| 字段 | 说明 |
|---|---|
| `sources` | 相对用例目录的源文件；`[]` 表示产物来自别处（见下） |
| `arch` | 展开为 `<id>-x64` / `<id>-x86` 两个测试 |
| `build` | 可选，追加编译参数；缺省用通用 flags（`-O2 -s -mwindows -luser32 -lgdi32`） |
| `seconds` / `timeoutMs` | 渲染秒数 / 超时；`-1` = 取 job 的 longSeconds（长跑用例） |
| `env` / `argv` | 测试专属诊断 env / 追加参数（其余产品语义 env 由 native 管线给） |
| `checks` | host 判定器引用（§8），缺省 `result-json` |
| `backend` | 可选：固定 d3d/dxvk 后端；缺省跑当前用户档位 |
| `matrix` | 可选：参数矩阵，见 §3.3 |

**产物来源不止一种**（现有事实，必须处理）：

- `sources` 有值 → 构建脚本用 mingw 交叉编译（如 `dns_probe.c`）；
- 产物来自 wine 构建系统 → 写 `"from_wine": "winehua_graphics_smoke"`（源码在
  `thirdparty/wine/programs/winehua_graphics_smoke/`，exe 由 wine 构建产出）；
- 产物来自 vkd3d → 写 `"from_vkd3d": "x64/winehua-d3d12-smoke.exe"`。

构建脚本按这三种 provider 取产物，统一放进 payload。

### 3.3 矩阵展开

```json
{
  "id": "dxvk-cube",
  "sources": ["winehua_d3d_switch_cube.c"],
  "arch": ["x64"],
  "matrix": {
    "d3dBackend": ["dxvk_legacy", "dxvk_modern_2_6"]
  }
}
```
展开为 `dxvk-cube-x64-le[gacy]` / `dxvk-cube-x64-mo[dern]` 两个测试实例。
参数组合不再复制粘贴条目。

### 3.4 suite 定义

```json
{ "name": "core", "title": "入口门禁", "tests": ["opengl-smoke", "dns-probe"] }
```

suite 只是用例的分组视图；`all` / `long` 这类组合套件由构建脚本按
"全部用例" / "标记 long 的用例"生成。

## 4. 载荷通道

### 4.1 构建产物（不进 HAP）

```
build/smoke-payload/
  suites.json          # 沿用现有 schema（设备端 Runner 直接可读）
  manifest.json        # 每个文件的 sha256 + buildId
  x64/*.exe  x86/*.exe
  assets/*
```

`assemble.sh` 删除 smoke 打包段；产品 HAP **不含任何测试资产**。
开发构建可用 `make hap SMOKE_PAYLOAD=1` 把 payload 打回 rawfile 作为兜底
（见 §4.3 优先级）。

### 4.2 推送

```bash
hdc file send -b app.hackeris.winehua build/smoke-payload \
    /data/storage/el2/base/files/smoke-payload
```

规则（实测）：**`-b <bundle>` 必须配沙箱视角 remote**（`/data/storage/el2/base/files/…`），
写真实路径会被当成相对路径落到不存在的位置。整目录递归送达。

### 4.3 设备端导入

`SmokeHook` 在引擎 ready 时（有 smoke 请求才做）：

1. 读 `files/smoke-payload/manifest.json` 的 `buildId`；
2. 与 `<prefix>/drive_c/smoke/.build-id` 比对，一致则跳过（正常会话零动作）；
3. 不一致 → 删旧树 → copyTree 到 `<prefix>/drive_c/smoke` → 写 `.build-id`。

兜底优先级：`files/smoke-payload`（推送）> HAP rawfile（开发构建时才存在）。
两个来源都没有 → 会话报 FAIL（`suites` 阶段），不阻塞产品会话。

### 4.4 备选通道（一期不做）

`hdc rport tcp:<port> tcp:<port>`（设备→host 反向转发）实测可用，配合 host 侧
HTTP 服务可做设备主动拉取。收益与推送区别不大（都要 hdc 在场），一期不引入。

## 5. 运行协议（job）

### 5.1 Want

```
aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode smoke \
  --ps winehua.job_file /data/storage/el2/base/files/smoke-job.json
```

`winehua.mode` 非 `smoke` 时产品行为零变化；`winehua.job_file` 指向推送来的 job 文件
（复杂 job），小 job 也可直接 `--ps winehua.job '<json>'`。

### 5.2 job 文件

```json
{
  "runId": "core-20260916-120000",
  "suite": "core",
  "prefix": "reuse",
  "longSeconds": 3600,
  "params": {
    "env": { "DXVK_HUD": "fps" },
    "argv": [],
    "d3dBackend": "dxvk_modern_2_6"
  }
}
```

三个互斥的选测方式（优先级从高到低）：

| 字段 | 用途 |
|---|---|
| `inline` | 内联测试定义数组（**调试临时用例**：exe 先推送到 C:\smoke，定义直接给 runner，不经过 suites.json） |
| `tests` | 按 testId 选测（如 `["opengl-x64"]`），跑 suite 的子集 |
| `suite` | 跑整个 suite |

`params` 对选中的所有测试生效（env 合并、argv 追加、后端覆盖）——
"参数组合调试"不需要改仓库任何文件。

### 5.3 结果

设备端不变形：`<prefix>/drive_c/smoke/results/<run-id>/<test-id>.json` +
`suite-summary.json`。host 轮询路径按 prefix 解析（复用当前设备上的 `.wine` 或 clean 后的新 prefix）。

## 6. 设备端改动

保持"物理隔离目录 + 少量标记钩子"的形态，改动集中在 `ets/smoke/`：

| 文件 | 改动 |
|---|---|
| `SmokeTypes.ets` | `SmokeRequest` 增加 `jobFile` / `inline` / `params` / `tests` 字段 |
| `SmokeHook.ets` | Want 解析 `winehua.job_file`；payload 导入源改为推送目录（带 buildId 比对）；删除 rawfile 播种路径（改兜底） |
| `SmokeRunner.ets` | 选测三方式 + params 合并（现有 suite 循环逻辑不变） |
| `SmokeDevPanel.ets` | 不变（侧边栏手动入口保留，跑 core） |

产品文件仍只有 7 处标记钩子，抄录（合并/摘除动作见 `docs/SMOKE_REBUILD_20260831.md` §11）：

| 文件 | 钩子 |
|---|---|
| `entryability/EntryAbility.ets` | `import { SmokeHook }` + `applyWant(parameters)` |
| `service/WineEnvService.ets` | `import` + init 内 `attach()` 判定 + `onNewWant()` + enterReady 链尾 `onEngineReady()` |
| `pages/Index.ets` | `import { SmokeDevPanel }` + 侧边栏 `<SmokeDevPanel />` |

## 7. host 工具（`automation/smoke.py`）

替代 `run_regression.py`。子命令：

| 命令 | 作用 |
|---|---|
| `build [--suite X] [--case X]` | 扫描 `smoke/tests/` → mingw 交叉编译 → `build/smoke-payload/`（秒级，无 Docker） |
| `install` | `hdc install` 当前 HAP（仅设备端代码变更时需要） |
| `push` | `hdc file send -b` 推 payload 到沙箱（旧目录先删，推后校验） |
| `run --suite core [--prefix clean] [--tests ID,ID] [--env K=V] [--inline FILE]` | 推 job + `aa start` + 轮询 + 归档 + 判定 |
| `check <run-dir>` | 只跑判定（对历史归档可重跑） |
| `gate` | 预定义门禁（core ×3 reuse + core ×1 clean） |
| `devices` | 列设备并选目标 |

环境要求（跨环境约束）：

- 必需：python3、hdc（`WINEHUA_HDC` 或 PATH）
- 仅 build 需要：mingw-w64 交叉编译器
- 仅视觉判定需要：numpy + pillow
- 设备地址：`--device` 显式给，或 `WINEHUA_DEVICE`，或 `hdc list targets` 单选时自动

归档结构：`build/automation-logs/<suite>-<runId>/`，含 `artifact.json`
（payload 版本 / device / longSeconds）与 `host-summary.json`（判定结论）。

## 8. 判定层

`automation/checks/`，一个判定器一个纯函数：

```python
def check(run_dir: Path, test: dict, ctx: dict) -> dict:
    """→ {"status": "PASS|FAIL|UNSUPPORTED", "stage": str, "message": str, "metrics": {...}}"""
```

内置：

| 判定器 | 内容 |
|---|---|
| `result_json` | 默认：读测试结果 JSON 的 status |
| `visual` | 现有 `validate_frame.py` 平移（固定帧四象限/立方体颜色） |
| `coverage` | 现有 `get_d3d11_coverage` 平移（DXVK 用例的 CPU readback / fallback 判定） |

用例的 `checks` 字段引用判定器；判定结果写 `<run-id>/host-summary.json`。
**判定与执行彻底分离**：`check <run-dir>` 可对任意历史归档重跑，
改判定规则不用碰设备。

## 9. 覆盖面

一期迁移现有全部套件（core / opengl / audio / d3d8 / d3d9 / wine-vulkan /
wine-vulkan-present / dxvk / dxvk-dynamic / dxvk-long / dxvk-modern-baseline /
dxvk-modern-long / gpu-diagnostics / dxvk26-requirements / d3d12），
保证替代后无能力退坡。

现成例子：`thirdparty/wine/programs/winehua_dinput_probe/`（输入探针）已写好源码与
Makefile，但因"接入要改 assemble 编译段 + suites 生成段"而未进任何套件——新方案下
它只需补一个 `smoke/tests/dinput-probe/test.json`（`"from_wine"` provider 指过去）。

后续用例积累方向（机制已就绪，按需加目录）：

| 方向 | 例子 |
|---|---|
| 输入链路 | 鼠标/键盘/滚轮注入（CLICK-PIPE/KBD-PIPE 端到端） |
| 合成/窗口 | 多窗口 z-order、子窗口/popup、resize |
| 生命周期 | prefix 首启 / 二启 / 重启引擎 / clean |
| 后端矩阵 | wined3d / dxvk_legacy / dxvk_modern × arch（matrix 展开） |
| 系统集成 | 文件/注册表/进程/DNS/HTTPS（schannel+gnutls 已有教训） |
| 稳定性 | 长跑（long 标记） |

## 10. 迁移路径（替代现有设施）

| 阶段 | 内容 | 验证 |
|---|---|---|
| P1 | 构建外置：`smoke/tests/` 目录化 + `smoke.py build` | 产物 exe 与原 `build/staging/wine-data/smoke/` 逐字节比对 |
| P2 | 传输/执行：`smoke.py push/run`；设备端读推送目录 | 推 payload + 手工 `aa start` 跑 core 全绿 |
| P3 | job 协议：选测 / params / inline；设备端 Runner 扩展 | `--tests opengl-x64`、params 覆盖 env 实测生效 |
| P4 | 判定层拆分：`automation/checks/` + `smoke.py check` | 对 P2/P3 归档重跑判定，结果一致 |
| P5 | 拆除：删 `run_regression.py`/`validate_frame.py`、assemble 的 C:\smoke 载荷段；文档收口 | 全量套件 14/15 PASS；`grep run_regression` 零代码引用 |

各阶段均已完成。`winehua.*` 的 Want 5 键（suite/run_id/prefix/long_seconds）
作为手动入口协议保留：不带 job 文件时 `aa start --ps winehua.mode smoke ...`
仍可直接起套件，见 `automation/README.md`。

## 11. 决策记录

1. **推送通道**：`hdc file send -b <bundle>` + 沙箱视角 remote 路径，实测可用
   （Pad 与开鸿 PC）。设备端 seed 同时保留 `files/wine/smoke`（HAP rawfile 解压树）
   作为离线备用源。
2. **payload 形态**：目录树（非 zip）——推送后设备端按 manifest 内容版本整树导入。
3. **侧边栏手动入口**：`SmokeDevPanel` 保留，同链起 core 套件。
4. **host 工具形态**：单文件 `automation/smoke.py` + `automation/checks/` 判定器包。
