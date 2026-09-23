# 常用命令速查

> 适用场景：日常开发中最常用的命令和路径，打开这一页就能找到。
> 最后核实：2026-09-22
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
$H shell "bm uninstall -n app.hackeris.winehua"
$H file send "entry/build/default/outputs/default/entry-default-signed.hap" "/data/local/tmp/winehua.hap"
$H shell "bm install -p /data/local/tmp/winehua.hap"
$H shell "rm -rf /data/app/el2/100/base/app.hackeris.winehua/files/.wine /data/app/el2/100/base/app.hackeris.winehua/files/wine"
$H shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

**增量部署**（只改了界面或原生代码，不用重装）：

```bash
$H shell "aa force-stop app.hackeris.winehua"
$H file send "entry/build/default/outputs/default/entry-default-signed.hap" "/data/local/tmp/winehua.hap"
$H shell "bm install -p /data/local/tmp/winehua.hap"
$H shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

设备地址和各自的状态见 [assets/devices.md](assets/devices.md)；也可以用 `hdc list targets` 看当前连了哪些设备。

## 看日志

```bash
# 实时日志（全部）
hdc -t <设备IP> hilog

# 实时日志（只看关心的标签）
hdc -t <设备IP> hilog | grep -E 'CLICK-PIPE|KBD-PIPE|WineWM|WL_Plugin|WL_Server|WL_Input|CRASH'

# 取最近的日志（缓冲区只保留最近几分钟）
hdc -t <设备IP> shell "hilog -z 500 -t app"

# 清空日志缓冲区，然后复现问题，这样抓到的都是干净的
hdc -t <设备IP> shell hilog -r

# Wine 自己的标准错误输出（每天一个文件）
hdc -t <设备IP> shell "cat /data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_$(date +%Y%m%d).log"

# 在里面搜关键词
hdc -t <设备IP> shell "grep -i '关键词' /data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_$(date +%Y%m%d).log"
```

注意：有的日志（例如 `[MUTEX-SPIN]`）只写进 Wine 标准错误文件，不会出现在 hilog 里。日志标签的完整列表和各自的坑见 [debugging/observability.md](debugging/observability.md)。

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
