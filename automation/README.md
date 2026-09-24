# WineHua 自动化回归测试

用例、套件、判定器都在仓库里；载荷随包分发（wine-data.zip 的 smoke/ 树），
开发环境由 host 经 hdc 推送更新（优先于包内版本），
判定在 host 侧做。设计见 `docs/engineering/testing-design.md`。

## 快速开始

```bash
make NATIVE_ARCH=arm64-v8a hap            # 1. 构建（项目根 Makefile 是唯一构建入口）
python3 automation/smoke.py install       # 2. 安装 HAP 到设备
python3 automation/smoke.py build         # 3. 构建测试载荷到 build/smoke-payload/
python3 automation/smoke.py run --suite core   # 4. 跑一个套件（自动推送 + 判定）
```

## 目录

| 路径 | 作用 |
|------|------|
| `smoke/tests/<case>/test.json` | 用例定义：exe 从哪来（源码交叉编译 / wine 产物 / vkd3d 产物）与 checks 声明 |
| `smoke/suites/<name>.json` | 套件定义：引用用例、按 arch 展开、env / backend / seconds 等参数 |
| `automation/smoke.py` | host 工具：build / push / run / check / install / devices |
| `automation/checks/` | 判定器（纯函数）：`result-json`、`visual:<validator>`、`coverage` |
| `entry/src/main/ets/smoke/` | 设备端编排：SmokeHook（请求消费 + 载荷播种）、SmokeRunner（执行）、SmokeDevPanel（侧边栏入口） |

## 载荷通道

```
smoke/tests + smoke/suites ──build──▶ build/smoke-payload/{x64,x86}/*.exe + suites.json + manifest.json
                                          │
                                    push（hdc file send -b）──▶ 沙箱 files/smoke-payload/
                                          │                        + 当前 prefix 的 C:\smoke
                                          ▼
                      设备端 SmokeHook.seed（引擎 ready 时）：按 manifest.suiteVersion 比对，
                      内容版本不同则整树导入 C:\smoke
```

- 载荷有**两个来源**，seed 按优先级取：`files/smoke-payload`（host 推送，开发环境改测试
  只推几 MB、不重装 HAP）＞ `files/wine/smoke`（**随包分发**——assemble 打进 wine-data.zip，
  发布环境无 host 也能播种，SmokeDevPanel 侧边栏入口可跑 core 自检）。`build/smoke-payload`
  不进版本库。
- `suites.json` 的 `suiteVersion` 是载荷内容哈希（定义 + 各 exe），任一处变化都会触发重新播种。
- 冷启动（prefix 未创建）只更新推送源，`C:\smoke` 由设备端 seed 播种；prefix 已存在时
  直接更新 `C:\smoke`，本次会话立即生效。

## 用例与套件

`test.json` 三选一提供 exe：

| 字段 | exe 来源 |
|------|---------|
| `build.sources/cflags/libs` | 用 mingw 交叉编译（`vulkan_import`/`dxvk_include` 可选） |
| `from_wine` | wine 构建的 `programs/<name>` 产物（含 i386/x86_64 两种架构） |
| `from_vkd3d` | vkd3d 构建根下的相对路径产物 |

`checks` 声明判定器（默认 `result-json`）；声明 `visual:<validator>` 的用例会在测试
写出 `"fixed-frame"` 结果后自动截取固定帧（多次尝试，任一帧通过即通过）。

套件条目字段：`case`（用例 id）、`id`/`testId`、`arch`、`env`、`backend.d3d`/`backend.dxvk`、
`seconds`、`timeoutMs`、`mode`、`extraArgs`、`argvMode`/`argv`。

`backend.dxvk` 只在 `d3d=vkd3d_limited_500k` 档被消费（`wine_env.cpp` 的
`AppendD3dBackendEnv` 用它选 DXVK overlay 与 `WINEHUA_DXVK_ROOT`）；其余档位走各自
分支，写不写都一样。声明了 `d3d` 就要一起声明 `dxvk`：不声明会退回"设备当前设置"，
而它由 `EntryAbility.defaultDxvkBackend()` 按机型与系统版本解析（命中已验证过的机型 +
系统版本组合才用 modern，其余 → legacy），同一套件在不同设备上测的
就不是同一个东西。

`env` 里的 `WINEDEBUG` 与 `WINEHUA_WINEDEBUG` 都不生效：前者被设备端显式忽略
（`wine_child.cpp` 的 profile 选择要自己判断 exe 类型），后者的读取早于 `__env` 应用
（同文件 458 行 vs 479 行，属实现缺陷）。`smoke.py` 装载套件时直接拦下这两类声明。
Wine 日志看 hilog 的 `WineChild-stderr` tag。

## 判定

设备端只产出原始数据（结果 JSON、固定帧），PASS/FAIL 由 `automation/checks/` 解释：
能力探针的 `UNSUPPORTED` 是合法答案，不会因为设备端 suite 汇总记 FAIL 而误判。
判定规则改了不用重跑设备：

```bash
python3 automation/smoke.py check build/automation-logs/<suite>-<runId>
```

## 命令

| 命令 | 说明 |
|------|------|
| `build [--suite NAME] [--case NAME] [--check DIR]` | 构建载荷；`--case` 可单独构建未接入套件的用例（如手动调试工具 win32-driver），`--check` 与既有 payload 逐字节比对 |
| `push` | 推送载荷到设备沙箱（`run` 会自动做） |
| `run --suite NAME` | 推送 + `aa start` + 轮询 + 回传归档 + 判定 |
| `check <run-dir>` | 对历史归档重跑判定 |
| `install [--hap PATH]` | 安装 HAP |
| `devices` | 列出 hdc 设备 |
| `gate` | 入口门禁：3×reuse core + 1×clean core（引擎健康与基础渲染的最小回归） |

`run` 的常用参数：`--prefix reuse\|clean`、`--tests ID,ID`（选测）、`--inline FILE`
（内联临时用例，exe 须已在 `C:\smoke`）、`--env KEY=VALUE`、`--d3d`、`--dxvk`、
`--seconds`、`--timeout-ms`、`--long-seconds`、`--device`、`--timeout-minutes`。

设备选择：`--device` → `WINEHUA_DEVICE` → `hdc list targets` 唯一设备。hdc 路径：
`WINEHUA_HDC` → PATH。

## 套件一览

- **core / opengl**：OpenGL smoke（x86+x64）固定帧 + 四象限视觉校验，最小回归门禁
- **audio / d3d8 / d3d9**：音频、D3D8 能力、D3D9 cube
- **wine-vulkan / wine-vulkan-present**：Wine Vulkan offscreen 探针 + present 链路
  （须走 vkd3d/dxvk 档，venus runtime env 只在这两档注入）
- **dxvk**：D3D11 完整功能矩阵（feature level、texture/descriptor/subresource/3D/UAV/BC、
  零 CPU readback/upload、无 WineD3D fallback）
- **dxvk-dynamic**：dynamic constant buffer 专项
- **dxvk-long / dxvk-modern-long**：长时间稳定性（默认 1 小时，`--long-seconds` 可调）
- **dxvk-modern-baseline**：DXVK 2.6.2 x86/x64 baseline + cube 回归
- **dxvk-500k-routes**：500k 混合路由的两条 DXVK 支路（1.10.3 = 全新设备默认，2.6.2 = UI 选过 DXVK 2.6.2 后的组合）。**当前已知红**：产品为保 D3D12 渲染注入 `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`（去掉则 D3D12 常驻 map 上传断、渲染黑屏），该开关会破坏 DXVK 支路的 compute 回读 → `computeUavFunctional=false` 是产品真实缺陷的如实反映，两难的治本方案与验证方案见 memory `persistent-map-sync-dxvk-readback`
- **gpu-diagnostics**：报告 Guest Vulkan、DXVK DLL 实际加载路径与 D3D11 device 状态
- **dxvk26-requirements**：DXVK 2.6.2 所需的 Guest/Wine Vulkan 1.3 transport 资格探针
- **d3d12**：VKD3D-Proton limited-500k 1000 帧图形 smoke（含 checkpoint 进度）+ gears/triangle demo 各 600 帧（`from_vkd3d` 用例，demo 侧 `--frames/--result` 由 patch 0019 提供）
- **all / long**：组合套件

## 结果归档

`--archive-root`（默认 `build/automation-logs/`）下按 `<suite>-<runId>/` 归档：

```
job.json                      # 下发给设备端的运行描述（选测/参数覆盖/内联用例）
suite-summary.json            # 设备端套件汇总（原始数据）
device-results/<testId>.json  # 各测试结果原样回传
frames/<testId>[-n].jpeg      # 固定帧截图（视觉判定用）
artifact.json                 # run 身份（payloadVersion、device、longSeconds）
host-summary.json             # 判定结论（per-test + suite 级）
```

退出码：判定层全 PASS 为 0，否则 1。

## 常见问题

**重建 HAP 后第一次跑，前半段很久没动静**

HAP 里引擎内容变了时，设备端不会直接启动引擎（正常使用要用户点「应用引擎更新
（重置）」才解压新引擎）。测试会话里这一步是自动的：SmokeHook 检测到
`upgradePending` 就自己走 `doReset`（停 → 清 → 重解压 → 重启，会清空 prefix），
然后接着跑测。多出来的时间是解压几百 MB 引擎数据，不是卡住。

**改了套件定义，设备上跑的还是旧行为**

`push`/`run` 推的是 `build/smoke-payload/`，改了 `smoke/tests/` 或 `smoke/suites/`
之后必须先 `smoke.py build` 重建载荷。工具会比对定义与载荷的时间戳，过期直接
报错拒绝执行（实测踩坑：改了套件档位没重建，设备端 suites.json 还是旧 backend，
测试结果完全没变）。

**视觉用例报 missing-frame**

graphics-smoke 的固定帧窗口是测试末尾 2 秒（`duration_ms - 2000` 起持续渲染同一帧），
host 必须在这段时间内轮询到 `"fixed-frame"` 结果并截图。轮询用一次 hdc 批量读文件
（`sandbox_texts`），避免逐文件往返把周期撑出窗口。

## 不侵入性

设备端 smoke 代码集中在 `entry/src/main/ets/smoke/`，产品文件里只有带
`// [[SMOKE]]` 标记的钩子行（`WineEnvService.attach/onNewWant/enterReady`、
`EntryAbility.applyWant`、`Index.<SmokeDevPanel/>`）。无 `winehua.mode=smoke`
请求时这些钩子是空操作。main-ui 合并时按
`docs/archive/SMOKE_REBUILD_20260831.md §11` 摘除整个目录与标记行。
