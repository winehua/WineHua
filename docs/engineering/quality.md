# 工程质量

> 适用场景：改完代码要验证时、提交之前、发版之前。
> 最后核实：2026-09-24
> 相关文档：[testing-design.md](testing-design.md)（测试设施的设计）、[testing-cases.md](testing-cases.md)（怎么写用例）、[workflow.md](workflow.md)（提交与合并）、[../build/guide.md](../build/guide.md)（构建）、[../build/release.md](../build/release.md)（打包发版）

## 改了什么，就跑什么

提交之前至少跑一轮相关的测试。不确定跑什么的时候，先跑 `core`（一分钟内出结果），再按需要扩大。

| 改动范围 | 构建 | 建议跑的套件 |
|---|---|---|
| `entry/src/main/ets/`（界面、服务） | `make NATIVE_ARCH=arm64-v8a hap` | core |
| `entry/src/main/cpp/`（合成、输入、渲染） | 同上 | core + wine-vulkan + dxvk |
| `thirdparty/wine/`（Wine 源码） | 完整 `make NATIVE_ARCH=arm64-v8a` | core + dxvk + d3d12 |
| DXVK legacy / modern | 完整构建 | dxvk / dxvk-modern-baseline |
| 只改 `smoke/tests/` 或 `smoke/suites/` | 不用重装 | `smoke.py build` 之后直接 run |
| 只改判定（`automation/checks/`） | 不用碰设备 | `smoke.py check <归档目录>` |

长跑套件（`dxvk-long`、`dxvk-modern-long`）默认跑一小时，平时不用跑，用 `--long-seconds 120` 做短验证就够。

**为什么改 Wine 源码必须完整构建**：合成阶段要把 `wine-data.zip` 重新打包，只做 `hap` 会跳过这一步，装上去的还是旧的引擎内容。

## 标准流程

```bash
make NATIVE_ARCH=arm64-v8a hap                  # 1. 构建
python3 automation/smoke.py install             # 2. 装到设备
python3 automation/smoke.py run --suite core    # 3. 跑套件
```

`run` 会自己把测试载荷推到设备、启动应用、轮询结果、把归档拉回来并给出结论，退出码为 0 表示全过。归档在 `build/automation-logs/<套件>-<runId>/`，`host-summary.json` 是结论，`device-results/` 是设备端的原始数据。

接了多台设备时加 `--device <ip:port>`，`smoke.py devices` 可以看当前连了哪些。

临时调试时 `run` 支持几个覆盖开关：

```bash
python3 automation/smoke.py run --suite dxvk --tests dxvk-legacy-x64      # 只跑某几个用例
python3 automation/smoke.py run --suite dxvk --d3d dxvk_modern_2_6        # 换后端
python3 automation/smoke.py run --suite dxvk --env WINEHUA_D3D11_DYNAMIC_CB=1   # 覆盖环境变量
python3 automation/smoke.py run --suite dxvk --seconds 30                 # 改时长
```

## 判定的原则

**设备端只负责跑，不负责判。** 设备端产出原始数据（结果 JSON、截图），通过还是失败由主机上的判定程序决定。

这样做的好处是：判定规则改了不用重跑设备，`smoke.py check <归档目录>` 可以对几个月前的结果重新判定。

判定器是 `automation/checks/` 下的纯函数，只读归档目录里的数据，**不依赖设备现场**——这是硬约束。

能力探针报 `UNSUPPORTED` 是合法答案，不算失败（比如设备本来就不支持某个特性，报不支持是对的）。

## 遇到时好时坏的失败

有几个用例本来就偶尔失败（比如 Venus 首次计算派发的读回竞态）。遇到失败先别改代码：

1. **查历史归档**（`build/automation-logs/`），看这个用例以前是不是也这样失败。
2. **同一个配置重复跑几次**，确认是随机的还是稳定复现。稳定复现的失败是有信息的，随机失败要另外想办法。
3. **看失败的位置是否固定**。比如总是失败在「该用例的第一个派发」，这本身就是定位线索。

两条不要做的事：不要把间歇失败当成「重跑一次就过了」放过去；也不要在没搞清楚原因之前就改判定条件让它变绿——**判定放宽会把真问题一起放过去**。项目里有过判定写得太松、把卡死和崩溃都判成通过的阶段，那是比失败更糟的状态。

## 自动化测试管不到的地方

**跑通自动化测试不等于功能可用。**

自动化用例测的是「点」上的能力——某个 D3D 特性能不能画对、某个 Vulkan 调用能不能读回——它们跑的都是专门的测试程序，行为规整。真实的 Windows 程序不是这样：长时间运行、消息和线程互相交织、大量测试程序根本不会出现的调用组合。

红警 2 就是例子：图形和输入相关的套件全绿，实际启动游戏却会卡死——问题出在测试程序覆盖不到的路径上。

所以定一条规矩：**涉及真实程序、输入操作、画面显示的改动，必须人工跑一遍。** 自动化测试是效率工具，不能替代人工验证。

## 构建脚本的规矩

构建链、转换链、集成链（两个以上步骤，或者要调外部命令的）在交付时要满足两条：

1. **命令固化进仓库脚本**，放在 `scripts/` 下，用相对路径，能直接执行。命令只存在于会话记录或者某个人的记忆里，等于不存在——需要重做的时候没人知道怎么跑。

2. **从零重建演练过一次**。删掉产物目录，从头跑一遍链，全绿，才算「可复现」。

脚本本身要守的几条：

- 开头 `set -eo pipefail`。管道会吞掉错误，不设这个的话，前面命令失败了脚本照样往下跑，最后打出「成功」。
- 每个阶段显式打印在做什么，关键产物做存在性检查（文件在不在、大小对不对、命令退出码），检查不过就非零退出，不许「构建失败还继续下一步」。
- 脚本头部的注释写清楚：正确命令、应该在哪个目录执行、产物路径、怎么用。把源码目录当构建目录用是这类脚本最常见的低级事故。
- 耦合点写进注释：跟 SDK 版本、路径、头文件行为、宏定义相关的点，写清楚「哪个版本、什么条件触发、将来升级时怎么检查」。

## 发布前的检查

打包和签名的具体操作见 [../build/release.md](../build/release.md)。发布前要确认的事：

1. **版本号已更新**（`AppScope/app.json5` 的 `versionCode` 和 `versionName`）——安装和升级都认这个号。
2. **相关套件全过**，并且做过人工验证（见上面那节）。
3. **打包配置正确**：调试配置和发布配置是两套文件，打包前替换、打完还原，别把发布配置留在工作区。
4. **产物是自己构建的**：确认设备上装的、要发布的包，来自本次构建，而不是上次留下的旧文件。

## 测试设施本身也要验证

改了测试设施（`automation/`、`smoke/`）之后，用门禁跑一遍确认设施本身是好的：

```bash
python3 automation/smoke.py gate    # 3 次复用环境 + 1 次全新环境跑 core
```

用例本身也是代码，加完要确认跑得通、判定符合预期再提交。
