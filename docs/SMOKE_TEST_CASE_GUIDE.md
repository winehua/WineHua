# Smoke 测试用例规范

这份文档讲**怎么写一个测试用例、怎么挂进套件、写完怎么验证**。照着做就能把一个新测试加进自动化回归。

三份文档的分工：

| 文档 | 回答的问题 |
|---|---|
| 本文 | 用例怎么写、有哪些字段和规矩 |
| `docs/SMOKE_V2_DESIGN.md` | 整套设施为什么这样设计 |
| `automation/README.md` | 日常怎么跑（命令、归档、判定输出） |

## 1. 一个用例由两部分组成

```
smoke/tests/<用例名>/test.json    ← 用例定义：exe 从哪来、叫什么、声明哪些判定
smoke/suites/<套件名>.json        ← 套件挂载：什么档位、什么参数、超时多少
```

两部分缺一不可：**没挂进任何套件的用例不会进载荷**，设备上根本没有它的 exe。要单独构建的话用 `python3 automation/smoke.py build --case <用例名>`（手动工具走这条）。

## 2. 用例定义：test.json

完整字段：

```json
{
  "id": "dns-probe",
  "title": "wine dnsapi 解析（含 NXDOMAIN 反例）",
  "exe": "winehua_dns_probe.exe",
  "arch": ["x64", "x86"],
  "build": { "...": "见下表" },
  "checks": ["result-json", "visual:rgba-quadrants"]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 用例名，和目录名一致 |
| `exe` | 是 | 载荷里的文件名 |
| `arch` | 是 | `x64` / `x86`，按声明的数量展开成多个测试 |
| `title` | 否 | 一句话说明测什么 |
| `build` / `from_wine` / `from_vkd3d` | 三选一 | exe 的来路，见下 |
| `checks` | 否 | 判定器列表，缺省 `["result-json"]`，见第 6 节 |

### exe 的三种来路

| 来路 | 写法 | 适用 |
|---|---|---|
| 源码交叉编译（最常用） | `"build": {"sources": ["smoke/xxx.c"], "cflags": [...], "libs": [...]}` | 新写的测试程序放 `smoke/` 目录下，用 mingw 编译。`dxvk_include`、`vulkan_import` 两个开关分别用于要 DXVK 头文件和要链 vulkan-1 的场合 |
| wine 构建产物 | `"from_wine": "winehua_xxx"` | 程序源码在 wine 子模块 `programs/` 下的 |
| vkd3d 构建产物 | `"from_vkd3d": "limited-500k/x64/gears.exe"` | vkd3d 构建根下的现成 exe |

选择顺序：能写独立 C 源就用 `build`；测试对象本身在 wine 或 vkd3d 源码树里，就用对应产物来路。产物来路的路径写错了构建会直接报缺失，不会静默跳过。

## 3. 测试程序要自己说清成败

设备端只负责跑，判断成败靠**你的程序写的结果 JSON**。判定器读不到合法结果就判 FAIL——所以「跑完但没写结果」等于失败，这是有意的（防止假绿）。

协议头：`thirdparty/wine/programs/winehua_smoke_protocol.h`，直接 include 使用。参考 `smoke/winehua_dns_probe.c`（最简样例）：

```c
#include "../thirdparty/wine/programs/winehua_smoke_protocol.h"

struct winehua_smoke_options options;
winehua_smoke_parse_options(&options, argc, argv, 6);  /* 最后一个参数是默认秒数 */

/* ... 跑测试，得到 status / message / metrics ... */

winehua_smoke_write_result(&options, status, "dns-api", message, metrics);
```

结果 JSON 的字段：

| 字段 | 要求 |
|---|---|
| `status` | 必须是终态之一：`PASS` / `FAIL` / `SKIP` / `UNSUPPORTED`。**`UNSUPPORTED` 是合法答案**——设备没有这个能力不算失败 |
| `stage` | 失败发生在哪个阶段（如 `dns-api`、`dxvk`），方便定位 |
| `message` | 一句话说清结果。失败信息要能区分「环境不可达」和「代码坏了」 |
| `metrics` | 数值指标（帧数、耗时、布尔矩阵……），coverage 判定靠它 |

长跑的测试建议跑测期间周期性写 `status=RUNNING` 的心跳快照——卡死被杀后盘上留下的是最后的心跳，判定器看到非终态会判 FAIL 并带上最后的 stage/message，比一句「结果文件缺失」有用得多。

## 4. 套件挂载：suites/<套件>.json

一个测试条目的完整写法：

```json
{
  "case": "d3d12-gears",
  "testId": "d3d12-gears-600f",
  "arch": ["x64"],
  "backend": {
    "d3d": "vkd3d_limited_500k",
    "dxvk": "dxvk_legacy"
  },
  "env": {},
  "argv": ["--frames", "600", "--run-id", "<run-id>", "--test-id", "<test-id>",
           "--result", "C:/smoke/results/<run-id>/<test-id>.json"],
  "argvMode": "raw",
  "seconds": 0,
  "timeoutMs": 120000
}
```

| 字段 | 说明 |
|---|---|
| `case` | 引用哪个用例 |
| `id` / `testId` | 二选一，语义不同：`id` 是**多架构展开**——`arch` 声明了几个架构就展开成几个测试，名字自动带后缀（`id: "dns-api"` + x64/x86 → `dns-api-x64`、`dns-api-x86`）；`testId` 是**单实例**——名字原样用，适合 x64-only 或名字里要带参数的（如 `d3d12-gears-600f`） |
| `backend.d3d` | **必须写**。不写就退回「设备当前设置」，它由机型决定——同一个套件在不同设备上测的就不是同一个东西 |
| `backend.dxvk` | `d3d=vkd3d_limited_500k` 时**必须一起写**（该档位下 dxvk 轴会被消费）；其他档位可省 |
| `env` | 诊断用的 env。有硬性限制，见第 5 节 |
| `argvMode` | 缺省 `std`：runner 自动生成协议参数（`--automation --run-id --test-id --result`，带 `--seconds`）；`raw`：原样用你写的 argv，`<run-id>` / `<test-id>` 占位符会被替换。自有参数体系的程序（d3d12 系列）用 `raw` |
| `mode` | `present`（默认，开窗口跑）/ `offscreen`（离屏，wine-vulkan 系用） |
| `seconds` | std 模式下传给程序的运行秒数；`-1` 表示用本次请求的长跑时长（默认 3600s） |
| `timeoutMs` | 设备端轮询超时，缺省 120000。**要给足程序实际运行时间**，正常跑完都到不了线 |

## 5. env 的规矩

- **`WINEDEBUG` 和 `WINEHUA_WINEDEBUG` 不能声明**：前者在 `__env` 通道里被显式忽略，后者读取时机早于 `__env` 应用，两个都到不了测试进程。`smoke.py` 装载套件时直接拦下报错。要看 Wine 日志，走 hilog 的 `WineChild-stderr` tag 或沙箱里的 stderr 文件。
- 可以声明的：测试程序自己认的诊断键（如 `WINEHUA_SMOKE_ASSETS`）、`--env` 覆盖用的产品键。
- 临时改参数不改仓库文件：`smoke.py run --suite X --env KEY=VALUE --d3d <档位>`，对选中的所有测试生效，用来复现特定参数组合。

## 6. 判定声明（checks）

| 判定器 | 干什么 |
|---|---|
| `result-json`（默认） | 读结果 JSON 的终态 status |
| `visual:<校验器名>` | 对测试末尾的固定帧截图跑像素校验。**帧只在最后 2 秒出现**，host 端已做批量轮询压周期，偶发 missing-frame 先看设备是不是卡了 |
| `coverage` | 套件级，检查一组功能矩阵是否都覆盖到（数据来自各测试的 metrics） |

判定器是纯函数、只读归档数据。改判定规则不用重跑设备：`python3 automation/smoke.py check <归档目录>` 对历史结果重新判定。

## 7. 独立性和可复现

硬性要求，违反了测试结果就不可信：

1. **不依赖上一个用例的残留**。每次 run 都可能换 prefix 状态（clean 编排会清盘），用例必须从零开始自建环境。
2. **不依赖当前时间、网络等外部状态**。必须依赖外部服务的（如 DNS），失败信息要能区分「服务不可达」和「代码坏了」。
3. **设备端只跑不判**。PASS/FAIL 由你的程序写结果、host 判定器下结论，别在 ArkTS 侧加判断逻辑。
4. **产品代码不引用测试 exe，载荷独立成树**。载荷随包分发（wine-data.zip 的 `smoke/` 树，经设备端 seed 播种到 `C:\smoke`），发布环境也能跑；开发环境 host 推送源优先、**先删后推**——载荷里没有的文件（比如你手动推进去的工具）会在下一次 run 时被清掉。想让工具常驻，就把它做成用例挂进套件（参照 `d3d12-gears`）。

## 8. 写完之后

```bash
# 1. 构建载荷（只构建某用例验证编译：--case）
python3 automation/smoke.py build

# 2. 跑一遍，确认跑得通、判定符合预期
python3 automation/smoke.py run --suite <套件名> --tests <testId>

# 3. 确认设备端结果文件内容（归档在 build/automation-logs/<套件>-<runId>/）
cat build/automation-logs/<套件>-<runId>/device-results/<testId>.json

# 4. 提交（用例目录 + 套件 json 一起）
```

**改了用例必须先跑一遍再提交**——用例本身也是代码，没跑过的用例进了套件就是给别人埋雷。

## 9. 常见坑

| 坑 | 说明 |
|---|---|
| 结果路径没写盘符 | `--result` 必须是 Windows 盘符形式（`C:/smoke/...`）。native 路径转的反斜杠形式没有盘符，wine 会解析到当前盘根下，结果落错目录，host 轮询永远超时（实测踩过：渲染 613 帧完成但判定超时） |
| 期望设备上有载荷外的文件 | 推送先删后推，没有的东西会被清掉 |
| `timeoutMs` 给太小 | 设备端正常跑完就超时了，失败信息只有一句轮询超时 |
| 把 `UNSUPPORTED` 当失败 | 能力探针报 UNSUPPORTED 是合法答案，判定不算 FAIL |
| 用 `seconds` 控制资源型超时 | `seconds` 是「跑多久」，`timeoutMs` 才是「等多久」。程序自己控制资源（帧数、迭代数）时 `seconds` 传 0 即可 |

## 10. 两个完整示例

**源码型 + std 模式**（`smoke/tests/dns-probe/test.json` + 套件条目）：

```json
{ "case": "dns-probe", "id": "dns-api", "arch": ["x64", "x86"],
  "backend": { "d3d": "wined3d" }, "seconds": 8, "timeoutMs": 120000 }
```

程序源码 `smoke/winehua_dns_probe.c` 里 `winehua_smoke_parse_options` 解析 runner 生成的协议参数，结束时 `winehua_smoke_write_result` 写终态。

**产物型 + raw 模式**（`smoke/tests/d3d12-gears/test.json` + 套件条目，见第 4 节的完整示例）：

程序自带参数体系（`--frames` 控制帧数、达标自动退出），argv 全部自己写，结果 JSON 由程序按协议字段自己拼。
