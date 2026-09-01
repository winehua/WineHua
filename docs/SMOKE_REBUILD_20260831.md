# Smoke 回归设施重建设计（一期）— 2026-08-31

> 依据 `docs/SMOKE_INFRASTRUCTURE.md` §4 三层重建设计，把 smoke 从「生产 UI 内嵌的调试设施」
> 收敛为「外部脚本驱动的快速自动化回归 + 应用内极简入口」。设计决策获用户确认：
> 触发 = 外部脚本（aa start Want 协议）+ 应用内极简入口；prefix = 复用 `.wine`（`--prefix clean`
> 临时重置）；一期范围 = 全 PE 套件；结果回传 = 文件 JSON 协议（沿用旧 C:\smoke\results 协议）。

## 1. 数据流

```
[host: WSL]                                     [设备: App 沙箱]
automation/run_regression.py
  ├─ make hap (Docker)                          EntryAbility.publishLaunchRequest
  ├─ hdc install                                 ├─ winehua.mode=smoke ──→ AppStorage
  ├─ aa start 带 Want (4 键) ───────────────────> ├─ winehua.suite / run_id / prefix
  ├─ 轮询 suite-summary.json <──────────────────└─ 'winehua.smoke.request'
  └─ hdc file recv 归档 + PASS/FAIL 判定
                                                WineEnvService.enterReady()
                                                waitForPrefix → seedSmokePayload
                                                → SmokeRunner.run(request)
                                                SmokeRunner.ets (瘦解释器)
                                                  1. 读 C:\smoke\suites.json
                                                  2. 逐个 runWineProgram
                                                  3. 轮询 results/<run-id>/<test-id>.json
                                                  4. 汇总 suite-summary.json
手动入口: Index 侧边栏「开发测试」区 → 同款 request (suite=core, run_id=manual-<ts>)
```

## 2. 设备端协议（Want，4 键）

| 键 | 值 | 说明 |
|---|---|---|
| `winehua.mode` | `smoke`（唯一认可值） | 缺省 = 正常会话，零行为变化 |
| `winehua.suite` | suite 名（suites.json 的键） | 一期：core/audio/d3d8/d3d9/wine-vulkan/dxvk/dxvk-long/dxvk-dynamic/gpu-diagnostics/dxvk26-requirements/dxvk-modern-baseline/dxvk-modern-long/d3d12/all/long |
| `winehua.run_id` | 任意 ASCII | 结果目录 `results/<run-id>/`；手动入口 `manual-<ts>` |
| `winehua.prefix` | `reuse`（默认）/ `clean` | `clean` → 引擎启动编排前置 `env.doReset()`（stopSession→wipe→自动 startSession 的现成编排；裸 resetWinePrefix 不先停引擎会损坏活 prefix） |

恢复不引入旧版全套：`experiment/game/click/long_seconds/perf_profile 白名单` 不进一期（文档 §3 点名砍掉）。
perf_profile 不走 Want——smoke 会话跑产品默认档（原生 `BuildSessionEnv` 无显式 profile 的缺省即
出厂基线）。**d3d 档位语义**：smoke 会话用当前持久档位（`winehua.d3d.preference`），脚本需要
出厂基线时显式传 `winehua.d3d_backend=dxvk_legacy`（publishLaunchRequest 已有该解析）——
runner 不做任何档位特殊逻辑。long 套件的时长由 host 脚本经 `winehua.long_seconds`（可选，默认 3600）。

## 3. suites.json（assemble.sh 生成，随载荷版本化）

与 `smoke/manifest.json` 同层（`wine_data/smoke/suites.json`），`suiteVersion` 共用。

```json
{
  "schemaVersion": 1,
  "suiteVersion": "phase2-vulkan-dxvk-v10-vkd3d-default",
  "suites": {
    "core": {
      "tests": [
        { "testId": "opengl-x64", "exe": "x64/winehua_graphics_smoke.exe", "env": {}, "seconds": 8, "timeoutMs": 60000 },
        { "testId": "opengl-x86", "exe": "x86/winehua_graphics_smoke.exe", "env": {}, "seconds": 8, "timeoutMs": 60000 }
      ]
    },
    "d3d12": {
      "tests": [
        { "testId": "d3d12-1000f", "exe": "x64/winehua_d3d12_smoke.exe",
          "argv": ["--frames", "1000",
                   "--result", "C:/smoke/results/<run-id>/<test-id>.json",
                   "--checkpoint", "C:/smoke/results/<run-id>/<test-id>.ckpt"],
          "env": {}, "timeoutMs": 180000 }
      ]
    }
  }
}
```

约定：
- `exe` 相对 `C:\smoke\` 根（`x64/…` / `x86/…`，**无** `smoke/` 前缀——载荷打包在
  `smoke/{x64,x86}`，播种到 `C:\smoke` 后即根级子目录；曾误编 `smoke/x64/…` 导致
  runner 拼成 `C:\smoke\smoke\x64\…` → wine `failed to open`）；x64/x86 arch 由路径表达
- `env` 只声明**测试专属诊断键**。产品语义（DXVK 稳定化注入、d3d 后端 overlay、perf profile、
  box64 档位）已由 native `BuildSessionEnv` + `runWineProgram(d3dBackend)` 参数收口，
  suites.json 不重复声明（双头维护消灭，见 _SMOKE_INFRASTRUCTURE §2 侵入点 3 的教训）。
  事实来源：旧 SmokeRunner（c2f0f7a^）的 PE env 矩阵逐条核对后遗留键，如
  `WINEHUA_SMOKE_ASSETS`（wine-vulkan/d3d12 用 assets）、`WINEDEBUG=+loaddll,+module`
  （fallback 检测）、dxvk-modern 的 trace 键等。
- `argv` / `seconds`（渲染秒数）/ `timeoutMs` 可选：秒数与超时对齐旧 Runner 规格
  （opengl=8s/60s、audio=3s/45s、d3d8/d3d9/dxvk=5~8s/180s、dxvk-long=longSeconds+90s、
  gpu-diagnostics/dxvk26=0s/90s）。`argv` 中 `<run-id>`/`<test-id>` 占位符由 Runner
  运行时替换。d3d12 的 exe 参数已对照 patch 0005 确认：`--frames`（默认仅 3！
  `VKD3D_GRAPHICS_SMOKE_DEFAULT_FRAMES 3u`，1000 帧须显式传）、`--result`、`--checkpoint`
  （后者二选一传 `--result` 即可）；另确认路径用正斜杠（Wine 的 fopen 接受 `C:/smoke/...`，
  规避 JSON/heredoc 双重转义）
- `seconds`/`timeoutMs` 为 `-1` 时表示「取请求的 longSeconds」（dxvk-long/dxvk-modern-long 用，
  请求默认 3600；`timeoutMs=-1` ⇒ `longSeconds*1000+90000`）
- `WINEHUA_SMOKE_RUN_ID / WINEHUA_SMOKE_TEST_ID` 由 Runner 固定注入，不在 suites 声明
- 新增测试 = 改 assemble 生成段 + smoke C 源码，不动 ArkTS

### suites.json 生成实现（assemble.sh）

在 `cat > "$smoke_dir/manifest.json"` 之后追加同名函数/段：`write_smoke_suites $smoke_dir $smoke_suite_version`，
逐一 heredoc 生成上表 15 个 suite。env 段为空对象的 suite 用最小 JSON（`"env": {}`）避免膨胀；
有诊断键的 suite（wine-vulkan、dxvk-modern-*、d3d12、gpu-diagnostics、dxvk26-requirements）
在生成段集中列出（占位符从旧 Runner 核对值更新）。

## 4. SmokeRunner.ets（新文件，瘦解释器）

`entry/src/main/ets/service/SmokeRunner.ets`，单例 + subscribe 镜像（与 WineEnvService 同款）。

```ts
export interface SmokeRequest { suite: string; runId: string; prefix: 'reuse' | 'clean'; source: 'want' | 'ui' }
```

职责（单星串行 async 编排，busy 防重入）：
1. `start(request)`：
   - 引擎就绪门槛：`prefixReady` 由调用方（WineEnvService enterReady 链 / Index 按钮使能）保证；
     runner 内部再校验一次，未就绪 → 记 FAIL 汇总退出
   - 进入 runner 时请求已被 WineEnvService 消费（§6），`prefix === 'clean'` 的重置
     发生在**引擎启动编排前置**（不是 runner 内部）——runner 只跑测试，无干净语义
   - `fs.mkdirSync(results/<run-id>)`（C 盘树内，recursive）
2. 读 `C:\smoke\suites.json`（沙箱 fs 路径 `FILES_BASE/.wine/drive_c/smoke/suites.json`）；
   缺文件/无 suite → 单条 FAIL 汇总（message 说明）
3. 逐 test：
   - env = `{WINEHUA_SMOKE_RUN_ID, WINEHUA_SMOKE_TEST_ID, ...suite env}`（结果协议键由 runner 控制）；
     d3dBackend：从 WineEnvService 当前状态取（getD3dBackend/getDxvkBackend —— smoke 跑在当前
     用户档位；脚本侧如需基线档位可通过 Want 的 d3d 参数（publishLaunchRequest 已支持
     winehua.d3d_backend）在启动时设定，不必在 runner 内建特殊逻辑）
   - `runWineProgram({windowsExePath: 'C:\\smoke\\<exe>', argv, environment, workingDirectory: 'C:\\smoke', d3dBackend, dxvkBackend})`
   - 轮询 `results/<run-id>/<test-id>.json`（间隔 500ms，止于 `timeoutMs`）：
     文件出现且 JSON 可解析后，`status` 为终态（PASS/FAIL/SKIP/UNSUPPORTED）→ 采结果；
     超时 → 构造 FAIL（stage=timeout，message 缺失原因）
   - 进度镜像：`statusText`（`Smoke: core 2/6 …`）、`lastResult`（`PASS 等`）
4. 汇总 `results/<run-id>/suite-summary.json`（旧协议：
   `{schemaVersion, runId, suite, status, tests:[{testId,status,stage,message,metrics}]}`，
   status = 全 PASS → PASS，否则 FAIL）；hilog tag `SMOKE` 打关键点
5. 手动入口（source=ui）同一条链，结果只写文件 + hilog，不弹 UI（侧边栏镜像状态行）

不重复实现：seed（WineEnvService 已做）、结果判定语义（host 脚本做权威判定——coverage/视觉）。

### 结果读取工具

runner 内部小工具（readTextIfExists / pathExists 风格），与 WineEnvService 同层，不 import
WineEnvService 私有成员（env 单例公开 getter：getD3dBackend/getDxvkBackend/resetWinePrefix）。

## 5. EntryAbility 恢复 Want 解析

`publishLaunchRequest` 在现有 d3d/dxvk 解析后追加：

```ts
const mode = value('winehua.mode', '');
if (mode === 'smoke') {
  AppStorage.setOrCreate<string>('winehua.smoke.request', JSON.stringify({
    suite: value('winehua.suite', 'core'),
    runId: value('winehua.run_id', `manual-${Date.now()}`),
    prefix: value('winehua.prefix', 'reuse') === 'clean' ? 'clean' : 'reuse',
    source: 'want'
  }));
}
```

`onCreate` / `onNewWant` 都走 publishLaunchRequest（已有）；smoke 请求只在本次会话发布后由
WineEnvService 消费一次（消费后清 AppStorage 键，避免热重启重触发）。

## 6. WineEnvService 触发接入

**请求消费单点**：`WineEnvService.consumeSmokeRequest()` 在两个入口调用——`init()`（冷启动）
与 `handleNewWant()`（App 存活时二次进入，由 EntryAbility.onNewWant 转发）。逻辑：

```ts
private consumeSmokeRequest(): void {
  const raw = AppStorage.get<string>('winehua.smoke.request');
  if (!raw) return;
  AppStorage.setOrCreate<string>('winehua.smoke.request', '');   // 清键: 不重触发
  this.pendingSmoke = JSON.parse(raw) as SmokeRequest;
  if (this.pendingSmoke.prefix === 'clean') {
    this.doReset();               // 现成编排: stopSession→wipe→自动 startSession;
                                  // 不能裸 resetWinePrefix (活引擎下删 prefix=损坏)
  }
}
```

**触发链**：`enterReady()` → `waitForPrefix().then(...)` 处，seedSmokePayload 之后追加
`void this.maybeRunSmoke();`——`maybeRunSmoke` 有 `pendingSmoke` 则
`SmokeRunner.getInstance().start(pendingSmoke)` 并置空 pending。
**引擎已就绪时的二次请求**（handleNewWant）：`pendingSmoke` 非空且 prefixReady → 直接触发 runner。
编排顺序：consume（clean→doReset 前置：停止→清空→自动重启）→ startSession → enterReady →
seed → maybeRunSmoke。Runner 只跑测试、不做 clean，语义单点。

## 7. UI 入口（Index.ets 侧边栏「开发测试」区）

「快速操作」区块之后插入（master 版单页布局，侵入收敛于一处）：

- 标题 `Text('开发测试')`
- `Button('Smoke 回归 (core)')`：`.enabled(!engineBusy && prefixReady)`，点击 → 
  `SmokeRunner.start({suite:'core', runId:'manual-'+Date.now(), prefix:'reuse', source:'ui'})`
- `Text(smokeStatusText)`：镜像 runner（aboutToAppear 订阅 `SmokeRunner.getInstance()`，
  `@State smokeStatus: string = ''`），运行中显示 `Smoke: 3/6 (opengl-x86)`，完成显示
  `Smoke PASS 6/6` / `Smoke FAIL 1/6 (…debug 文件路径)`

## 8. run_regression.py 改造（host 侧）

1. `start_args` 新协议：
   `--ps winehua.mode smoke --ps winehua.suite <s> --ps winehua.run_id <id> --ps winehua.prefix <reuse|clean>`
   （删除旧的 run_id/suite/prefix/long_seconds/perf_profile 用法——long_seconds 保留为可选键）
2. 轮询路径：`files/.wine/drive_c/smoke/results/<run-id>/suite-summary.json`（复用前缀；
   host-vulkan 的 `automation/results` 路径删除）
3. `--prefix clean`：提交 Want `winehua.prefix=clean`，由设备端 resetWinePrefix 处理
   （不再 hdc shell rm——沙箱权威性由 App UID 保证）
4. SUITES 收敛为 15（见 §3）；删除 `capabilities / vkd3d-capability` 及
   `write_capability_matrix / write_vkd3d_capability_decision / _vkd3d_*` 函数、
   host-vulkan 分支（`save_probe_results` 仅保留 per-test 归档）
5. 保留：`validate_frame.py`（固定帧视觉）、`get_d3d11_coverage`（dxvk 套件判定）、
   `capture_frame/capture_d3d11_frame`、artifact 校验（REQUIRED_PAYLOAD 增加 `smoke/suites.json`
   及 d3d12 条目）、--gate（core×3 reuse + 1 clean）
6. `dxvk_tests_for_suite` 对齐新测试 id（dxvk-legacy-x64/x86、dxvk-cube-x64、dxvk-modern-*-x64/…、
   dxvk-dynamic-cb-*）——以 suites.json 实际 id 为准

## 9. automation/README.md 同步

套件表收敛、参数表更新、Want 协议说明、二期预告（venus/capabilities/host-vulkan 依赖已删除的
native probe，另立；win32_driver game 点击驱动二期）。

## 10. 验证

1. `make NATIVE_ARCH=arm64-v8a hap`（arkts + assemble）通过；unzip wine-data.zip 确认
   `smoke/suites.json` 与 manifest 同层生成
2. 行为回归（模拟器或无设备时静态核对）：正常首启/二启无 smoke 日志与请求消费；
   文件浏览器启动 exe 路径不变
3. 设备验证（部署后）：`aa start --ps winehua.mode=smoke --ps winehua.suite=core --ps winehua.run_id=r1`
   → 设备端 hilog `SMOKE` 打点、`C:\smoke\results\r1\suite-summary.json` 生成 → host
   `run_regression.py --suite core` 过一遍；d3d12 套件独立跑一次（1000 帧）

## 11. 与 main-ui 的合并策略（2026-09-01 物理隔离重构后）

> 用户决策：smoke 是开发测试设施，**不得进入 main-ui**（面向用户的产品分支）。
> 上文 §4-§7 是重构前的接入形态（smoke 代码长在 WineEnvService/Index/EntryAbility
> 共享文件里，合并冲突高危）；2026-09-01 重构后 smoke ArkTS **物理隔离**进
> `entry/src/main/ets/smoke/`，行为语义不变（§6 的编排时序逐条平移）。

### 重构后的结构

```
entry/src/main/ets/smoke/          # 全部 smoke ArkTS (整目录可删)
  SmokeTypes.ets                   # SmokeRequest 接口
  SmokeRunner.ets                  # 瘦解释器 (跑套件/轮询结果/汇总)
  SmokeHook.ets                    # 枢纽: Want 解析 applyWant / 请求消费 attach
                                   # / clean 编排触发 / prefixReady 边沿 / 载荷播种
  SmokeDevPanel.ets                # 侧边栏「开发测试」区组件 (自镜像, Index 零状态)
```

公共产品文件仅剩 **带 `// [[SMOKE]]` 标记的行级钩子**（3 个文件，共 7 处
标记行）：

| 文件 | 钩子 | 摘除后 |
|---|---|---|
| `entryability/EntryAbility.ets` | `import { SmokeHook } ...` + `applyWant(parameters)` | Want 不再解析 smoke 键 |
| `service/WineEnvService.ets` | `import { SmokeHook } ...` + ① init 内 `if (attach(this)) return;`（clean 编排接管判定）② `onNewWant()` 内 `SmokeHook.onNewWant();`（二次 Want 消费）③ `enterReady` 链尾 `SmokeHook.onEngineReady();`（跑测触发，旧 maybeRunSmoke 同位置） | 初始化编排不含 smoke |
| `pages/Index.ets` | `import { SmokeDevPanel } ...` + `<SmokeDevPanel />` | 侧边栏无「开发测试」区 |

**触发语义**（与重构前 maybRunSmoke 逐条对齐，设备实测 4 场景过：
正常首启零干扰 / 冷启动 want / App 存活二次 want / clean 重建）：
- 冷启动：`attach` 只消费请求（clean 则编排并 return），跑测等 `enterReady` 链尾
  `onEngineReady`（引擎真 ready + waitForPrefix 完成；回放式 ready 不可信——
  broker 未就绪时 runWineProgram 返回 -1）
- 二次 Want：`onNewWant` 消费后立即跑（引擎必然已就绪，无常驻轮询）
- **SmokeHook 不订阅 env 状态镜像**：clean 编排启动瞬间引擎仍短暂 ready
  （stopSession 异步），订阅兜底触发会把 clean 请求当 reuse 提前跑（r6 实测误触发）

（`WineEnvService.cleanPrefixAndRestart` 由旧 `resetSmokePrefix` 改名而来，是**引擎级
通用能力**（只清 prefix 不删运行时，与 doReset 的区别），无 smoke 字眼——main-ui
可保留（无调用方则编译面为空）或随摘除删除。`FILES_BASE/WINE_ROOT` 加 export 供
SmokeHook 播种使用，原值不变。）

### 合并动作清单（main-ui 开发者在 merge master 后执行）

```bash
git rm -r entry/src/main/ets/smoke/          # 1. 整目录拔除
grep -rn '\[\[SMOKE\]\]' entry/src/main/ets/ # 2. 定位各标记行 → 逐处删除
grep -rn 'SmokeHook\|SmokeDevPanel' entry/src/main/ets/  # 3. 校验零引用
```

批量校验：`grep -rni 'smoke' entry/src/main/ets/` 应只剩注释级历史提及。
**main-ui 一期遗留**（应用库「Graphics Smoke / D3d Smoke」条目 + WineEnvService
`seedSmokePayload` 播种）同批清理，否则条目存在但 C:\smoke 无人播种（启动报
wine failed to open）。

### 数据/脚本层（不带 ArkTS 侵入，随合并安全）

`automation/`、`scripts/assemble.sh`（suites.json 生成段）、`wine_data/smoke/`（载荷）、
`docs/` 均为宿主侧/打包资产，不产生 App 行为面。合并后 main-ui 的 wine-data.zip
仍含 `smoke/` 载荷目录（静态资产，约几 MB，无 UI/运行时行为），可留可裁——
留可保证两分支 zip 产物结构一致，裁需在 assemble.sh 加开关（多一个分支差异点）。
