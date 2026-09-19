# Smoke 与回归测试

自动化回归设施在 `automation/` 和 `smoke/` 下，完整参考见 `automation/README.md`。
本文件只管两件事：**什么时候该跑什么**，以及**写用例时要守什么规矩**。

## 改了什么，就跑什么

提交前至少跑一轮相关套件。不确定跑什么时，先跑 core（一分钟内出结果），再按需扩。

| 改动范围 | 构建 | 建议套件 |
|---|---|---|
| `entry/src/main/ets/`（界面、服务） | `make NATIVE_ARCH=arm64-v8a hap` | core |
| `entry/src/main/cpp/`（合成、输入、渲染） | 同上 | core + wine-vulkan + dxvk |
| `thirdparty/wine/`（Wine 源码） | 完整 `make NATIVE_ARCH=arm64-v8a` | core + dxvk + d3d12 |
| DXVK legacy / modern | 完整构建 | dxvk / dxvk-modern-baseline |
| 只改 `smoke/tests/` 或 `smoke/suites/` | 不用重装 | `smoke.py build` 后直接 run |
| 只改判定（`automation/checks/`） | 不用碰设备 | `smoke.py check <归档目录>` |

长跑套件（`dxvk-long`、`dxvk-modern-long`）默认一小时，平时不必跑，用
`--long-seconds 120` 做短验证即可。

## 标准流程

```bash
make NATIVE_ARCH=arm64-v8a hap                  # 1. 构建（改过 ArkTS/C++/wine 才需要重装）
python3 automation/smoke.py install             # 2. 装到设备
python3 automation/smoke.py run --suite core    # 3. 跑套件
```

`run` 会自己把载荷推到设备、起 App、轮询结果、回传归档、给出判定结论，
退出码 0 表示全过。归档在 `build/automation-logs/<套件>-<runId>/`，
`host-summary.json` 是判定结论，`device-results/` 是设备端原始数据。

接了多台设备时加 `--device <ip:port>`；`smoke.py devices` 可以看当前连了哪些。

## 加一个用例

一个用例 = `smoke/tests/<名字>/test.json` 一个目录。exe 有三种来路，按情况选一种：

- `build.sources`：用 mingw 交叉编译 `smoke/` 下的 C 源（最常用）
- `from_wine`：wine 构建出来的 `programs/<名字>` 产物
- `from_vkd3d`：vkd3d 构建根下的产物

然后把它挂进套件 —— `smoke/suites/<套件>.json` 的 `tests` 里引用用例 id。
没挂进套件的用例不会进载荷（要用的话得 `smoke.py build --case <id>` 单独构建）。

用例要能自己说清楚成败：写结果 JSON（协议见
`thirdparty/wine/programs/winehua_smoke_protocol.h`），带 status / stage / message /
metrics；需要看图判的，在 `checks` 里声明 `visual:<校验器名>`。

## 判定怎么写

判定器是 `automation/checks/` 里的纯函数，读归档目录里的数据出结论：

- `result-json`：看设备端结果（默认）
- `visual:<校验器>`：对固定帧截图跑像素校验
- `coverage`：suite 级，检查一整套功能矩阵是否都覆盖到

**判定只读数据，不依赖设备现场** —— 这是硬约束。守住它，改判定规则就不用重跑设备，
`smoke.py check <归档目录>` 能对几个月前的结果重新判定。

## 几条规矩

1. **产品不为测试让步。** 产品代码不引用测试 exe，测试程序不进 wine 的 `bin/`；
   载荷走 wine-data.zip 的 `smoke/` 独立树（约 5 MB），经设备端 `SmokeHook.seed`
   播种到 `C:\smoke`——发布环境无 host 也能跑自检（SmokeDevPanel 侧边栏入口）。
2. **开发环境改测试不重装 HAP。** host 推送源 `files/smoke-payload` 优先于包内
   版本，`smoke.py run` 自动推几 MB 载荷即生效；重装 344 MB 只在引擎内容变化时发生。
3. **用例要独立、可复现。** 不依赖上一个用例的残留，不依赖当前时间/网络等外部状态；
   必须依赖外部服务时（如 DNS、HTTPS），失败信息要能区分"服务不可达"和"代码坏了"。
4. **设备端只跑不判。** 设备端产出原始数据，PASS/FAIL 由 host 决定。能力探针报
   `UNSUPPORTED` 是合法答案，不算失败。
5. **改测试后先跑一遍再提交。** 用例本身也是代码，加完要确认跑得通、判定符合预期。
6. **套件要钉死档位。** 声明了 `backend.d3d` 就一起声明 `backend.dxvk`（该轴只在
   `vkd3d_limited_500k` 档被消费，其他档位可省）。不声明会退回"设备当前设置"，而它由
   机型与系统版本决定 —— 同一个套件在不同设备上测的不是同一个东西。
   `WINEDEBUG` / `WINEHUA_WINEDEBUG` 都不能声明：设备端读不到（前者被显式忽略，后者
   读取早于 `__env` 应用），`smoke.py` 装载套件时直接拦下。Wine 日志看 hilog 的
   `WineChild-stderr` tag。

## 临时调试

改一行代码就得跑整套太慢时，`run` 支持几个覆盖开关：

```bash
# 只跑套件里的某几个用例
python3 automation/smoke.py run --suite dxvk --tests dxvk-legacy-x64

# 换后端 / 覆盖 env / 改时长，用来复现特定参数组合
python3 automation/smoke.py run --suite dxvk --d3d dxvk_modern_2_6
python3 automation/smoke.py run --suite dxvk --env WINEHUA_D3D11_DYNAMIC_CB=1
python3 automation/smoke.py run --suite dxvk --seconds 30

# 跑一个临时用例（exe 得已经在设备 C:\smoke 里，比如用 --case 构建过的）
python3 automation/smoke.py run --suite core --inline /tmp/my-case.json
```

`win32-driver` 这类手动工具不产结果 JSON、进不了套件，用
`smoke.py build --case win32-driver` 单独构建后再手动 `aa start` 调用。

## 常见问题

**重建 HAP 后第一次跑，前半段很久没动静。**
HAP 里引擎内容变了时，设备端不直接启动引擎（正常使用要人工点「应用引擎更新（重置）」）。
测试会话里这一步是自动的：SmokeHook 检测到 `upgradePending` 就自己走 `doReset`
（停 → 清 → 重解压 → 重启，会清空 prefix），再接着跑测。多等的是解压时间，不是卡住。

**视觉用例报 missing-frame。**
graphics-smoke 的固定帧只在测试最后 2 秒出现，host 得在这个窗口内轮询到结果并截图。
轮询已经改成一次 hdc 批量读文件（`sandbox_texts`）来压低周期；如果还偶发，先看设备
是不是卡了。设备端结果是 PASS 就说明渲染没问题，别去改测试程序。

**想确认设施本身还好不好。**
`python3 automation/smoke.py gate` 跑 3 次 reuse core + 1 次 clean core，
是引擎健康和基础渲染的最小门禁。
