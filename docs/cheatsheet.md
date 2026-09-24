# 常用命令速查

> 适用场景：日常开发中最常用的命令和路径，打开这一页就能找到。
> 最后核实：2026-09-24
> 详细说明见 [build/guide.md](build/guide.md)（构建）、[debugging/observability.md](debugging/observability.md)（日志）。

## 构建

项目根目录的 `Makefile` 是唯一的构建入口。下面命令都在项目根目录执行。

```bash
# 完整构建（改了 wine 源码、或不确定改了什么时用这个）
make NATIVE_ARCH=arm64-v8a

# 只改了界面代码（entry/src/main/ets/）或原生代码（entry/src/main/cpp/）
make NATIVE_ARCH=arm64-v8a hap

# 只构建某个阶段
make NATIVE_ARCH=arm64-v8a deps      # 交叉编译依赖
make NATIVE_ARCH=arm64-v8a wine      # Wine 与 wineserver
make NATIVE_ARCH=arm64-v8a box64     # box64（仅 arm64 需要）
make NATIVE_ARCH=arm64-v8a native    # 原生合成器依赖
make NATIVE_ARCH=arm64-v8a assemble  # 组装打包目录
make NATIVE_ARCH=arm64-v8a hap       # 打出 HAP 包

# x86_64 目标
make NATIVE_ARCH=x86_64
```

改了 `thirdparty/wine/` 下的 C 代码**必须**跑完整构建，因为最后的 assemble 阶段要重新打包 wine-data.zip。

## 部署到设备

先设好设备地址（把 `<设备IP>` 换成实际地址）：

```bash
H="hdc -t <设备IP>"
```

**完整部署**（改过 Wine 之后用，会清空 Wine prefix）：

```bash
$H shell "bm uninstall -n app.hackeris.winehua"   # 卸载会把应用数据（含 Wine prefix）一起清掉
$H file send "entry/build/default/outputs/default/entry-default-signed.hap" "/data/local/tmp/winehua.hap"
$H shell "bm install -p /data/local/tmp/winehua.hap"
$H shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

**只想清空引擎数据、不想重装**：用应用里的「重置 Wine 引擎」（等价于恢复出厂，会重新解压）。

**不要用 `hdc shell rm -rf` 删沙箱里的路径**——会被 SELinux 拒绝（`Permission denied`，真实路径和沙箱视角路径都一样）。清数据的正规途径只有上面两条。

**增量部署**（只改了界面或原生代码，不用重装）：

```bash
$H shell "aa force-stop app.hackeris.winehua"
$H file send "entry/build/default/outputs/default/entry-default-signed.hap" "/data/local/tmp/winehua.hap"
$H shell "bm install -p /data/local/tmp/winehua.hap"
$H shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

应用有四个 ability，**`aa start` 只能拉起导出的 `EntryAbility`**——`DesktopAbility` 等会报 `10103001 Failed to verify the visibility`。形态（虚拟桌面 / 多窗口）由应用自己按设备决定。

设备地址和各自的状态见 [assets/devices.md](assets/devices.md)；也可以用 `hdc list targets` 看当前连了哪些设备。

## 看日志

```bash
# 实时日志（全部）
hdc -t <设备IP> hilog

# 只看本项目的（-e 匹配消息正文，-T 匹配标签、可多选）
hdc -t <设备IP> hilog -e winehua
hdc -t <设备IP> hilog -T WL_Server,WineChild,WL_EGL

# 取最近的日志（缓冲区只保留几千行，约几分钟）
hdc -t <设备IP> shell "hilog -z 500"

# 落盘采集：清空 → 后台收 → 复现问题 → 拉回来分析
hdc -t <设备IP> shell "hilog -r"
hdc -t <设备IP> shell "setsid sh -c 'hilog -t app > /data/local/tmp/capture.log 2>&1' < /dev/null > /dev/null 2>&1 &"
hdc -t <设备IP> file recv /data/local/tmp/capture.log case-1.log

# Wine 自己的标准错误输出（每天一个文件）
hdc -t <设备IP> file recv -b app.hackeris.winehua /data/storage/el2/base/temp/wine_stderr_$(date +%Y%m%d).log ./wine_stderr.log

# 拉回本地后搜关键词
grep -i '关键词' wine_stderr.log
```

`hilog -t app` 不是按包名过滤——它选的是"应用类日志"这个大类型，会把设备上所有应用的日志一起打出来。精确过滤用 `-e`（消息正文）或 `-T`（标签）。落盘命令的三个重定向别省，少了 hdc 命令不返回（采集其实在工作）。

## 看现场

没有日志时（进程卡住、要确认版本、查崩溃历史）：

```bash
hdc -t <设备IP> shell "ps -ef | grep -i winehua"                  # 进程列表
hdc -t <设备IP> shell "hidumper -p <pid>"                          # 线程列表（带线程名）
hdc -t <设备IP> shell "hidumper --cpuusage"                        # 系统负载与 CPU 分项
hdc -t <设备IP> shell "aa dump -l"                                 # 任务栈与前台状态
hdc -t <设备IP> shell "bm dump -n app.hackeris.winehua"            # 版本、装机路径、调试/市场包
hdc -t <设备IP> shell 'hidumper -s 1201 -a "-p Faultlogger -l"'    # 崩溃日志清单
```

进程活着但不动时，先分清"自旋"还是"阻塞"（时间在涨 + 状态 `R` = 自旋；不涨 + 状态 `S` = 阻塞）：

```bash
hdc -t <设备IP> shell "cat /proc/<pid>/stat | cut -c1-60"          # 第 3 个字段是状态
hdc -t <设备IP> shell "A=\$(cut -d' ' -f14,15 /proc/<pid>/stat); sleep 3; \
                       B=\$(cut -d' ' -f14,15 /proc/<pid>/stat); echo \$A; echo \$B"
```

（`/proc/<pid>/maps`、`mem` 从外面读不到，地址性质要在进程内查。）自旋时的现场采集见 [debugging/fault-forensics.md](debugging/fault-forensics.md)。

注意设备上的 shell **不做通配符展开**（`/proc/<pid>/task/*/stat` 这类写法会失败），也没有 `awk`。崩溃记录的完整取法见 [debugging/observability.md](debugging/observability.md)。

## 抓运行中进程的调用栈

进程**卡死**（不是崩溃）时，设备上没有 gdb，`/proc` 也拿不到栈——从外部采样：

```bash
hdc -t <设备IP> shell "hiperf record -p <pid> -d 5 -s dwarf -o /data/local/tmp/cs.data"
hdc -t <设备IP> shell "hiperf report -i /data/local/tmp/cs.data -s"
```

输出是带占比的调用链树。进程卡住时采样点全落在同一条链上，**哪一层占 100% 就是卡在哪**。`-s dwarf` 不能省（不加只出热点函数），`-p` 只对调试包有效。详见 [debugging/observability.md](debugging/observability.md)。

注意：设备上的文件用 `hdc file recv` 拉回本地再分析，`/proc` 下的实时内容才用 `hdc shell "cat ..."`。**沙箱里的路径要加 `-b <包名>`、写沙箱视角**，而且这条通道要求设备上装的是调试包（市场版会报 `Invalid bundle name`）。路径换算规则见 [architecture/platform-ohos.md](architecture/platform-ohos.md) 的沙箱一节。Wine 的 stderr 也会转发到 hilog（标签 `WineChild-stderr`），但转发不保证可靠。日志标签的完整列表见 [debugging/observability.md](debugging/observability.md)。

## 自动化测试

```bash
# 看当前连了哪些设备
python3 automation/smoke.py devices

# 把测试程序装到设备
python3 automation/smoke.py install

# 跑一个套件（core 最快，一分钟内出结果）
python3 automation/smoke.py run --suite core

# 只跑套件里的某几个用例
python3 automation/smoke.py run --suite dxvk --tests dxvk-legacy-x64

# 跑多台设备时指定设备
python3 automation/smoke.py run --suite core --device <IP:端口>

# 不用重新跑，只对已有结果重新判定
python3 automation/smoke.py check build/automation-logs/<套件>-<runId>
```

改了哪部分代码该跑哪些套件，见 [engineering/quality.md](engineering/quality.md)。

## 关键路径

| 用途 | 路径（hdc 视角） |
|---|---|
| 应用根目录 | `/data/app/el2/100/base/app.hackeris.winehua/` |
| Wine prefix | `.../files/.wine/` |
| Wine 程序数据 | `.../files/wine/bin/` |
| Wine 标准错误日志 | `.../temp/wine_stderr_YYYYMMDD.log` |
| 缓存（标记文件、渲染日志） | `.../cache/` |

应用内部代码看到的路径不一样，两者的对应关系见 [debugging/observability.md](debugging/observability.md) 的路径映射表。

## 常用日志标签（速记）

| 标签 | 看什么 |
|---|---|
| `WL_Server` | 合成器主循环、帧提交 |
| `WL_Plugin` | 画面承载组件（XComponent）的注册与销毁 |
| `WL_EGL` | 渲染器初始化与渲染循环 |
| `WL_Input` | 事件注入、丢帧统计 |
| `WineWM` / `WWA` | 窗口生命周期（界面层） |
| `CLICK-PIPE` / `KBD-PIPE` | 鼠标、键盘事件从界面层到原生的完整链路 |
| `MW-*` | 合成细节（`MW-RNDR` 渲染、`MW-TAKE` 合成输出、`MW-SUBSURF` 子画面、`MW-MOVE` 窗口拖动） |
| `WL-ERR` / `CRASH` | 协议错误、进程崩溃 |
| `WineChild-stderr` | Wine 子进程的标准错误转发 |

完整的标签表见 [debugging/observability.md](debugging/observability.md)。
