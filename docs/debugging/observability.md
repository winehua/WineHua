# 日志与观测

> 适用场景：排查任何问题时，先看这篇搞清楚"从哪里能看到什么"。日常命令速查见 [../cheatsheet.md](../cheatsheet.md)。
> 最后核实：2026-09-24
> 相关代码：各模块的 `OH_LOG` 调用；日志开关见 `entry/src/main/cpp/common/perf_utils.h`

## 日志通道

### 1. hilog（设备系统日志）

所有界面层和原生代码打的日志都汇总到这里。

```bash
hdc -t <设备IP> hilog                  # 实时阻塞读，Ctrl-C 停
hdc -t <设备IP> shell "hilog -x"       # 一次性 dump 整个缓冲区后退出
hdc -t <设备IP> shell "hilog -z 500"   # 只取缓冲区尾部 500 行
```

**过滤参数**（可以组合，下面这几个实测有效）：

| 参数 | 作用 |
|---|---|
| `-T <标签>` | 按标签过滤，**子串匹配**，最多 10 个（逗号分隔，`^` 前缀表示排除） |
| `-e <正则>` | 按**消息正文**过滤（标签不参与匹配） |
| `-P <pid>` | 按进程过滤，最多 5 个（同样支持 `^` 排除） |
| `-L <级别>` | 按级别过滤（`E`/`W`/`I`/`D`/`F`，可多选，也认长名 `ERROR`）。**只有它不支持 `^` 排除**——帮助里写了，设备上会报 `Invalid log level` |

**注意 `-t app` 不是按包名过滤**：它选的是"应用类日志"这个大类，会把设备上所有第三方应用的日志一起打出来。只看本项目的日志要用 `-e winehua`（进程名会出现在每行里）或者 `-T` 列出关心的标签。

```bash
# 只看本项目的日志
hdc -t <设备IP> hilog -e winehua

# 只看合成器和 Wine 子进程
hdc -t <设备IP> hilog -T WL_Server,WineChild,WL_EGL

# 崩溃相关（正文里出现这些词）
hdc -t <设备IP> hilog -e 'CRASH|WL-ERR|SIGSEGV|SIGABRT'
```

**四个必须知道的坑**：

1. **缓冲区很小**。实测全量也就八千多行，按正常刷屏速度只够几分钟。要留证据必须落盘：

   ```bash
   hdc -t <设备IP> shell "setsid sh -c 'hilog -t app > /data/local/tmp/capture.log 2>&1' < /dev/null > /dev/null 2>&1 &"

   # —— 采集期间在设备上复现问题 ——

   hdc -t <设备IP> file recv /data/local/tmp/capture.log case-1.log
   ```

   三重定向不能省。只重定向 `hilog` 的 stdout 的话，它的 stderr 还挂在 hdc 的连接上，**命令不会返回**（采集其实一直在工作，很容易误判成失败然后重复启动）。

2. **浮点数字会被脱敏**。格式串里写 `%.2f` 但不加 `%{public}`，输出会变成 `<private>`，数字就看不到了。写日志时注意。
3. **`-D` 按 domain 过滤用不了**：三种写法都报 `Invalid domain string [CODE: -5]`，别在这上面花时间。
4. **pid 是动态的**。进程每轮引擎会话都换代，`-P` 记下的 pid 过一会就失效；日志分段要靠 `=== PID=... ===` 这类标记认，不能靠时间戳。

### 2. Wine 标准错误文件

Wine 内部（以及 box64）的输出写在这个文件：

```bash
hdc -t <设备IP> file recv -b app.hackeris.winehua /data/storage/el2/base/temp/wine_stderr_$(date +%Y%m%d).log ./wine_stderr.log
```

（沙箱里的文件都要走 `-b` 通道、路径写沙箱视角；它**要求设备上装的是调试包**，装的是应用市场版时会报 `Invalid bundle name`，直接 `hdc shell cat` 沙箱路径也会被 SELinux 拒。见 [../architecture/platform-ohos.md](../architecture/platform-ohos.md) 的沙箱一节。）

**这里有什么**：Wine 的 TRACE/ERR 日志、box64 崩溃现场（寄存器、地址）。

**坑**：
- 文件**按天命名、追加写入**，每次引擎会话开头会写一行 `=== PID=... ===` 作分隔。它不会自动清理，排查时认准自己那次会话的 PID 段。
- 同样的内容**也会转发到 hilog**（标签 `WineChild-stderr`），但代码注释里写明"hilog 转发实际不可靠"，所以排障以文件为准，两边都看。
- Wine 的 TRACE 日志默认关闭（`WINEDEBUG=-all`），需要输出时用 `fprintf(stderr, ...)`。

### 3. 渲染器宿主日志

virgl 宿主侧的诊断（fence 对账、buffer 轨迹）：

```bash
hdc -t <设备IP> file recv -b app.hackeris.winehua /data/storage/el2/base/cache/winehua_virgl_host.log ./winehua_virgl_host.log
```

每次应用启动重写，时间戳从启动算起。**这是判断"渲染卡死"的关键证据**：如果这个文件不再更新、但 guest 进程还活着、hilog 里的帧提交日志也停了，说明渲染线程卡死了。

### 4. 系统崩溃记录

崩溃日志存在 faultlog 目录里，`hdc shell` 直接读目录会被拒（`Permission denied`），但可以**通过 hiview 服务取出来**——底层是 `hidumper -s 1201`，秒级返回：

```bash
# 列出崩溃日志（文件名含包名、pid、时间）
hdc -t <设备IP> shell 'hidumper -s 1201 -a "-p Faultlogger -l"'

# 只看本项目的
hdc -t <设备IP> shell 'hidumper -s 1201 -a "-p Faultlogger -m app.hackeris.winehua"'

# 读某一次的完整内容
hdc -t <设备IP> shell 'hidumper -s 1201 -a "-p Faultlogger -f cppcrash-app.hackeris.winehua-20020230-20260918235134236.log"'
```

内容里有崩溃信号和地址（`Reason:Signal:SIGSEGV(SEGV_MAPERR)@0x...`）、故障线程的 tid 和名字、进程存活时长、调用栈，还有当时的版本号。崩溃日志是持久化的，隔一天再查也在——**判断"这个崩溃是不是新出现的"，看文件名里的时间和版本就知道**。

**box64 下的 Wine 进程栈通常只有 `[Unknown]`**：它是转译执行的，宿主符号对不上 guest 代码。这类崩溃要靠 Wine 标准错误文件里的 box64 现场（寄存器和地址），或者转成代码审查。

需要整个系统快照时用 `hdc -t <设备IP> bugreport ./br.txt`（12 MB 左右，等一两分钟），里面也含崩溃段。

### 5. 性能数据文件

应用自己写的性能数据：

| 文件 | 内容 | 打开方式 |
|---|---|---|
| guest ring 性能统计 | Venus 命令提交的耗时分类 | `VN_WINEHUA_PERF_SUMMARY=1` + `MESA_LOG_LEVEL=debug` |
| 显示帧率 | 每个窗口实际显示的帧率，每秒一行 | 默认写 `.../drive_c/windows/temp/winehua_display_fps.txt` |

设备上还有系统自带的采样工具 `hiperf`，不用改代码、不用加开关就能从外部采——看哪个线程在烧 CPU 时特别顺手，用法见 [performance.md](performance.md)。

### 6. 进程现场

没有日志时，从系统里直接看：

```bash
# 进程列表（含 box64 下的各个子进程）
hdc -t <设备IP> shell "ps -ef | grep -i winehua"

# 线程列表（内部就是 ps -efT，一次列全，带线程名）
hdc -t <设备IP> shell "hidumper -p <pid>"

# Unix socket 连接状态（第 6 列是状态：3=监听、1=已连接）
hdc -t <设备IP> shell "cat /proc/net/unix"

# 进程的真实身份（box64 下进程名会被改，这里看得到 wineserver 之类）
hdc -t <设备IP> shell "head -1 /proc/<pid>/status"

# 系统负载与 CPU 分项
hdc -t <设备IP> shell "hidumper --cpuusage"

# 任务栈、前台状态、Mission ID
hdc -t <设备IP> shell "aa dump -l"
```

**注意**：

- **设备上的 shell 不做通配符展开**，`cat /proc/<pid>/task/*/stat` 会报文件不存在。要看全部线程就用上面的 `hidumper -p`，或者自己写循环：

  ```bash
  hdc -t <设备IP> shell "ls /proc/<pid>/task/ | while read t; do cat /proc/<pid>/task/\$t/stat; done | cut -d' ' -f1-3"
  ```

- 设备上的工具是精简版：**没有 `awk`**（用 `cut`），**没有 `bugreport`**（那是 hdc 侧的子命令，见上面"系统崩溃记录"）。
- 鸿蒙没有 `/proc/<pid>/syscall`，`wchan` 也恒为 0。

### 7. 应用状态与版本

```bash
# 应用信息：版本号、装机路径、是调试包还是市场包
hdc -t <设备IP> shell "bm dump -n app.hackeris.winehua"

# 停掉整个应用（主进程和所有子进程一起清）
hdc -t <设备IP> shell "aa force-stop app.hackeris.winehua"

# 重新拉起
hdc -t <设备IP> shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

`bm dump` 输出里的 `appProvisionType`（`debug` / `release`）决定了能不能用 `-b` 通道读沙箱，见 [../architecture/platform-ohos.md](../architecture/platform-ohos.md) 的沙箱一节。

应用有四个 ability，**只有 `EntryAbility` 是导出的**——`DesktopAbility`、`VirtualDesktopAbility`、`WineWindowAbility` 用 `aa start` 拉会报 `10103001 Failed to verify the visibility`。形态（虚拟桌面 / 多窗口）由应用自己按设备决定，命令行只能拉起入口。

### 8. 运行中进程的调用栈

崩溃有 Faultlogger 记录，但**卡死**没有——进程活着，只是不动了。这种情况用 `hiperf` 从外部采：

```bash
hdc -t <设备IP> shell "hiperf record -p <pid> -d 5 -s dwarf -o /data/local/tmp/cs.data"
hdc -t <设备IP> shell "hiperf report -i /data/local/tmp/cs.data -s"
```

输出是带占比的调用链树，从根一路展开到具体函数。**进程卡住时采样点会全部落在同一条链上**，所以几秒的采样就等于"卡住那一瞬间的栈"——哪一层占到 100%，卡点就在哪。用法和限制（`-p` 只对调试包有效等）见 [performance.md](performance.md)。

另一条不用改代码的路子是**等系统自己记录**：应用主线程无响应超时后，系统会把所有线程的栈 dump 进 faultlog。`hidumper -e --list <进程名>` 能列出历史异常记录（`CppCrash`、`JsError`、`LowMemoryKill`、`SwapFull`），带时间和 `record_id`，再用 `hidumper -e --print <record_id>` 取详情。前提是进程真的卡到被判超时。

**这些途径都拿不到栈时的兜底**（下面的都实测过，结果一并列出）：

| 想试的 | 实测结果 |
|---|---|
| `/proc/<pid>/stack` | Permission denied |
| `/proc/<pid>/stat` 的 kstkeip / kstkesp | 恒为 0（鸿蒙不填这两个字段） |
| `/proc/<pid>/wchan` | 恒为 0 |
| `hidumper -p <pid>` | 只有线程名和启动时间，没有栈 |
| 设备上的 gdb / lldb / eu-stack / pstack | 都没有 |

从外部拿不到栈时，往进程内部走：信号现场、地址归因、guest 寄存器还原、卡死看门狗、最小复现探针——这套做法整理在 [fault-forensics.md](fault-forensics.md)。

剩下的办法是**在代码里打点**，从日志反推位置。要在设备上真的排查过这类问题（两套翻译器抢同一个故障槽，60 秒内循环 28 万次），要点是：

- **打点带地址和归属**：记录 PC、目标地址，以及这个地址属于谁（哪个模块、哪个注册过的槽）。光有"进了某个函数"不足以判断，地址归属才是关键。
- **从重复次数看循环**：正常路径不会几万次刷同一行。日志暴涨（一分钟几十 MB）本身就是信号。
- **写文件不写 stderr**：应用里的 stderr 走管道，进程一死管道内容就丢。调试输出要直接落盘，写完就关文件。
- **定位完就清掉**：这些是临时设施，不留在产品路径上。

## 日志标签

原生代码的日志用编译期定义的标签（`LOG_TAG`），界面层用消息前缀。按模块分：

| 模块 | 标签 | 看什么 |
|---|---|---|
| 合成器主循环 | `WL_Server` | 协议主循环、帧提交 |
| 窗口协议 | `WL_Xdg` | 窗口创建、尺寸限制、模态 |
| 输入 | `WL_Input`、`WL_PtrExt`、`WL_Seat` | 事件注入、相对指针、键盘焦点 |
| 输入法 | `WL_TextInput` | 文本输入协议 |
| 渲染 | `WL_EGL` | 渲染循环、帧耗时统计 |
| 图形后端 | `WL_GFX`、`virgl-child`、`virgl-presenter`、`venus-presenter` | 图形链路各段 |
| 画面组件 | `WL_Plugin` | XComponent 注册与渲染器管理 |
| 进程 | `WL_NAPI`、`WineChild`、`WL_SPAWN`、`WL_Broker` | 进程启动、崩溃检测 |
| 音频 | `WL_AUDIO` | 音频链路 |
| 手柄 | `WineGamepad`、`CtrlHub` | 手柄链路 |
| 帧率 | `WL_FPS` | 每 10 秒一次帧率摘要 |

合成器内部靠**消息前缀**再细分（因为协议层统一用 `WL_Server` 标签）：`[MW-RNDR]` 渲染、`[MW-TAKE]` 取帧、`[MW-SUBSURF]` 子表面、`[MW-MOVE]` 窗口拖动、`[MW-GEO]` / `[MW-RESIZE]` 几何与尺寸映射、`[MW-POPUP]` 弹出层、`[MW-COMMIT]` 帧提交、`[MW-NAPI]` 界面层转发。

界面层标签：`WWA`（窗口生命周期）、`WineWM`（窗口管理）、`CLICK-PIPE`（鼠标链路）、`KBD-PIPE`（键盘链路）。

常用的组合过滤（用设备端的 `-e` 先筛一道，省得把无关日志都传到本地；要接管道做二次统计再在本地用 `grep`）：

```bash
# 合成器的几何与尺寸映射（消息前缀，方括号要转义）
hdc -t <设备IP> hilog -e '\[MW-GEO\]|\[MW-RESIZE\]'

# 输入链路（按标签）
hdc -t <设备IP> hilog -T WL_Input,WL_PtrExt,CLICK-PIPE

# 丢帧统计（每 60 秒汇总一次）
hdc -t <设备IP> hilog -e 'Input-DROP'
```

## 沙箱路径对照

应用代码里的 `/data/storage/el2/...` 和 hdc 看到的 `/data/app/el2/100/base/app.hackeris.winehua/...` 是同一份数据，完整的换算规则和路径表在 [../architecture/platform-ohos.md](../architecture/platform-ohos.md) 的沙箱一节。排查时常用的几个：

| 应用内视角 | hdc 视角 | 用途 |
|---|---|---|
| `/data/storage/el2/base/files/` | `/data/app/el2/100/base/app.hackeris.winehua/files/` | Wine prefix、Wine 数据 |
| `.../temp/` | `.../temp/` | Wine 标准错误日志 |
| `.../cache/` | `.../cache/` | 渲染日志、零拷贝标记 |
| `/storage/Users/currentUser/Download/` | `/storage/media/100/local/files/Docs/Download/` | 下载目录（Wine 的 `Z:` 和 `HOME`） |

**读写沙箱都走 `-b <包名>` 通道**（`hdc file send|recv -b ...`），路径写沙箱视角——这条通道要求设备上装的是调试包。相关的坑（推目录会嵌套、`hdc shell` 读写删沙箱路径都会被拒）也见那一节。

## 崩溃定位

按这个顺序走：

1. **先看是整组挂了还是单个进程挂了**。每隔几秒统计一次进程数：如果应用和所有 Wine 进程一起消失，是宿主崩溃；只有某个 Wine 进程消失，是 guest 崩溃。

2. **查 Faultlogger**（见上面"系统崩溃记录"）。信号、故障线程、进程存活时长都在一条记录里，而且是持久化的，隔一天再查也在。

3. **看 Wine 标准错误文件的结尾**。如果有崩溃现场（box64 的 SIGSEGV 信息、寄存器和地址），就定位到 guest 侧；如果文件是干净收尾的，问题在宿主。

4. **在 hilog 里看进程是怎么没的**。搜 `signal:`——这个字段来自系统的 AppMS 和应用的 `ProcessMgr`，记录每个 native 子进程退出时的信号：`signal:11` 是 SIGSEGV，`signal:0` 是正常退出。（hilog 里**没有** `cppcrash` 字样；缓冲区只留几分钟，要事后查还得靠前两步。）

5. **拿不到栈就转代码审查**。重点看锁的使用（`*Locked` 后缀的函数要求持有锁）、跨线程共享数据的访问（锁外解引用指针成员容易变悬垂）、以及"同一个文件的另一条路径为什么是安全的"（差异点通常就是缺陷点）。

box64 下 Wine 进程崩溃的额外手法：从 box64 崩溃信息里的地址和寄存器值推算（比如地址差值是 ASCII 字符，说明文件内容被当成了指针），再用 `addr2line` 反查具体位置。

## 采集纪律

**一次运行采集下来的数据，要足够事后充分分析**——不是出了问题临时抓一点看看。

卡死、偶发崩溃、画面异常这些，往往不是想复现就能复现。抓的时候图省事、只收了自己以为相关的那几条，事后发现关键的一段没收进来，就只能重来一遍——而重来可能再也碰不上。所以采集按「这是我唯一一次机会」来准备。

### 先落盘，别边采边看

缓冲区只保留两三分钟；实时盯着刷屏看，看到的也只是当前假设里的那几个标签，真正的原因常常在你没看的标签里。先全量收进文件，事后离线过滤，代价只是几兆磁盘。

```bash
hdc -t <设备IP> shell "hilog -r"                    # 先清空，抓到的都是从零开始的
hdc -t <设备IP> shell "setsid sh -c 'hilog -t app > /data/local/tmp/capture.log 2>&1' < /dev/null > /dev/null 2>&1 &"

# —— 采集期间在设备上复现问题 ——

hdc -t <设备IP> file recv /data/local/tmp/capture.log case-1.log   # 拉回本地分析
```

（落盘命令的三个重定向别省，理由见上面 hilog 那节的坑 1。采集完记得把设备上的 `hilog -t` 进程杀掉——它会一直跑到重启。）

要留下分析就用 `file recv` 拉回本地——走 `hdc shell` 的 stdout 会经过终端层（换行可能被改写），文件大了也容易出问题。

### 一次带全，几路一起收

不同来源的信息不重合，缺哪一路事后都补不上：

| 来源 | 为什么 |
|---|---|
| hilog 全量（先别按标签掐） | 界面层和原生代码的日志都在这里 |
| 沙箱的 Wine 标准错误文件 | box64 崩溃现场在这里最完整 |
| 渲染器宿主日志（`cache/winehua_virgl_host.log`） | 图形问题必收，判断渲染卡死的关键证据 |
| 进程现场（`ps`、`/proc/net/unix`、线程状态） | 问题发生时进程还活着的话，这是唯一的现场快照 |
| 问题相关的诊断开关 | **要在启动前设好**——帧级开关是启动时读一次的，出了事再开来不及 |

### 记下当时的配置

分支（`master` 还是 `main-ui`）、版本号、D3D 档位、设备形态、prefix 是新建的还是沿用的。同一个现象在不同档位下原因可能完全不同——隔一天回头看，没人记得当时跑的是哪一套。

### 记下操作和日志的对应

复现步骤写成文字、标上时间；采集开始和结束、每次操作的前后，留个标记。否则事后面对几万行日志，没法切分"哪一段是这次操作产生的"。

### 采样范围要心里有数

移动类日志按 1/120 抽样，被抑制的输入（窗口不可见时的丢弃日志）同样抽样——**日志里没有不等于没发生**。要分析这些路径，得先去代码里去掉抽样条件再采一次（在 `input_manager.cpp` 里搜 `% 120 ==`）。参考：抽样是为防刷爆缓冲区，一次拖动十分钟能产生十几万行。

### 关键节点主动 flush

进程崩溃时，没来得及写出去的内容会丢。自己写调试输出时，写完就关文件（见 [../architecture/platform-ohos.md](../architecture/platform-ohos.md) 的经验 5）。

### 跨进程对齐靠标识，不靠时间戳

宿主、应用主进程、wine 子进程的时间戳不同源，对不齐。要认日志内容里的 PID、session 标识这类标记。

## 采集方法

排查输入类、交互类问题的流程：

1. 按上面的纪律采下来（清空缓冲区 → 全量落盘 → 复现 → 结束采集）。
2. **每个问题场景单独一个文件**，几件事不要混在一个日志里。
3. 结束前再 dump 一次缓冲区（`hilog -z 8000`），补上可能漏掉的尾部。
4. 分析时按链路分段过滤（见上面的标签表）。

**一条经验**：死锁类问题，"卡死前的最后一条日志"就是最后一跳——问题就在它后面那一步。
