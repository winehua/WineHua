# 日志与观测

> 适用场景：排查任何问题时，先看这篇搞清楚"从哪里能看到什么"。日常命令速查见 [../cheatsheet.md](../cheatsheet.md)。
> 最后核实：2026-09-22
> 相关代码：各模块的 `OH_LOG` 调用；日志开关见 `entry/src/main/cpp/common/perf_utils.h`

## 日志通道

### 1. hilog（设备系统日志）

所有界面层和原生代码打的日志都汇总到这里。

```bash
hdc -t <设备IP> hilog                      # 实时
hdc -t <设备IP> shell "hilog -z 500 -t app"  # 取最近 500 行应用日志
```

**三个必须知道的坑**：

1. **缓冲区很小**。日志量大的时候（比如每秒上百条帧日志），缓冲区只能保留两三分钟的内容。要留证据必须落盘：

   ```bash
   hdc -t <设备IP> shell "setsid sh -c 'hilog -t app > /data/local/tmp/capture.log' &"
   ```

2. **浮点数字会被脱敏**。格式串里写 `%.2f` 但不加 `%{public}`，输出会变成 `<private>`，数字就看不到了。写日志时注意。
3. **标签过滤抓不到时，按进程过滤**：`hilog -z N -P <pid>`。

### 2. Wine 标准错误文件

Wine 内部（以及 box64）的输出不在 hilog 里，写在这个文件：

```bash
hdc -t <设备IP> shell "cat /data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_$(date +%Y%m%d).log"
```

**这里有什么**：Wine 的 TRACE/ERR 日志、box64 崩溃现场（寄存器、地址）、`[MUTEX-SPIN]` 这类只在 stderr 打的诊断。

**坑**：
- **每次引擎会话会重写**这个文件，要跨会话留证据得先拷出来。
- 有些诊断（如 `[MUTEX-SPIN]`）**只写这个文件**，hilog 里搜不到——曾经因为搜错地方误判"没有自旋"。
- Wine 的 TRACE 日志默认关闭（`WINEDEBUG=-all`），而且 release 构建里 TRACE 可能被编译优化掉，需要输出时用 `fprintf(stderr, ...)`。

### 3. 渲染器宿主日志

virgl 宿主侧的诊断（fence 对账、buffer 轨迹）：

```bash
hdc -t <设备IP> shell "cat /data/app/el2/100/base/app.hackeris.winehua/cache/winehua_virgl_host.log"
```

每次应用启动重写，时间戳从启动算起。**这是判断"渲染卡死"的关键证据**：如果这个文件不再更新、但 guest 进程还活着、hilog 里的帧提交日志也停了，说明渲染线程卡死了。

### 4. 系统崩溃记录

系统的崩溃日志（faultlog）应用没有权限直接读，要从 bugreport 的相应段落里找。如果拿不到栈，就转成代码审查（看锁的使用、看共享数据的访问）。

### 5. 性能数据文件

| 文件 | 内容 | 打开方式 |
|---|---|---|
| guest ring 性能统计 | Venus 命令提交的耗时分类 | `VN_WINEHUA_PERF_SUMMARY=1` + `MESA_LOG_LEVEL=debug` |
| 显示帧率 | 每个窗口实际显示的帧率，每秒一行 | 默认写 `.../drive_c/windows/temp/winehua_display_fps.txt` |

### 6. 进程现场

没有日志时，从系统里直接看：

```bash
# 进程列表
hdc -t <设备IP> shell "ps -ef | grep -i winehua"

# Unix socket 连接状态（第 6 列是状态：3=监听、1=已连接）
hdc -t <设备IP> shell "cat /proc/net/unix"

# 线程状态（S=可中断睡眠、D=不可中断、R=运行）
hdc -t <设备IP> shell "cat /proc/<pid>/task/*/stat | cut -d' ' -f1-3"
```

**注意**：设备上的工具是精简版（没有 awk），要用 `cut`。另外鸿蒙没有 `/proc/<pid>/syscall`，`wchan` 也恒为 0。

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

合成器内部靠**消息前缀**再细分（因为协议层统一用 `WL_Server` 标签）：`[MW-RNDR]` 渲染、`[MW-TAKE]` 取帧、`[MW-SUBSURF]` 子表面、`[MW-MOVE]` 窗口拖动、`[MW-LIMITS]` 尺寸限制、`[MW-COMMIT]` 帧提交、`[MW-RAISE]` 置顶诊断。

界面层标签：`WWA`（窗口生命周期）、`WineWM`（窗口管理）、`CLICK-PIPE`（鼠标链路）、`KBD-PIPE`（键盘链路）。

常用的组合过滤：

```bash
# 输入链路全貌
hdc -t <设备IP> hilog | grep -E '\[PIPE\]|\[Input\]|\[PtrExt\]'

# 崩溃相关
hdc -t <设备IP> hilog | grep -E 'CRASH|WL-ERR|SIGSEGV|SIGABRT'

# 丢帧统计（每 60 秒汇总一次）
hdc -t <设备IP> hilog | grep 'Input-DROP'
```

## 沙箱路径对照

| hdc 视角 | 应用内视角 | 用途 |
|---|---|---|
| `/data/app/el2/100/base/app.hackeris.winehua/` | `/data/storage/el2/base/` | 应用根目录 |
| `.../files/` | `.../files/` | Wine prefix、Wine 数据 |
| `.../temp/` | `.../temp/` | Wine 标准错误日志 |
| `.../cache/` | `.../cache/` | 渲染日志、零拷贝标记 |
| `/storage/media/100/local/files/Docs/Download/app.hackeris.winehua/` | `{Z:}` | 用户下载目录（Wine 的 Z 盘和 HOME） |

**往沙箱里传文件**只有一条通道：`hdc file send -b <包名> <本地> <沙箱视角路径>`。相关的坑（推目录会嵌套、`hdc shell` 删除是假成功）见 [../architecture/platform-ohos.md](../architecture/platform-ohos.md) 的沙箱一节。

## 崩溃定位

按这个顺序走：

1. **先看是整组挂了还是单个进程挂了**。每隔几秒统计一次进程数：如果应用和所有 Wine 进程一起消失，是宿主崩溃；只有某个 Wine 进程消失，是 guest 崩溃。

2. **看 Wine 标准错误文件的结尾**。如果有崩溃栈（box64 的 SIGSEGV 信息），就定位到 guest 侧；如果文件是干净收尾的，问题在宿主。

3. **在 hilog 里找崩溃信号**。搜 `cppcrash` 或 `signal:`：
   - `exit with signal:11` 是崩溃的那个进程（`signal:9` 是被系统连带清理的）。
   - 崩溃处理线程的 `tid` 可以用来反查日志标签，确定崩溃线程的身份。

4. **拿不到栈就转代码审查**。重点看锁的使用（`*Locked` 后缀的函数要求持有锁）、跨线程共享数据的访问（锁外解引用指针成员容易变悬垂）、以及"同一个文件的另一条路径为什么是安全的"（差异点通常就是缺陷点）。

box64 下 Wine 进程崩溃的额外手法：从 box64 崩溃信息里的地址和寄存器值推算（比如地址差值是 ASCII 字符，说明文件内容被当成了指针），再用 `addr2line` 反查具体位置。

## 采集方法

排查输入类、交互类问题的标准流程：

1. 部署好后先清空缓冲区（`hilog -r`），这样抓到的都是从零开始的。
2. 后台持续采集，**每个问题场景单独一个文件**：

   ```bash
   hdc -t <设备IP> hilog 2>/dev/null | grep --line-buffered winehua > /tmp/case-1.log
   ```

3. 复现问题，然后结束采集。保险起见再 dump 一次缓冲区（`hilog -z 8000`）补上可能漏掉的尾部。
4. 分析时按链路分段过滤。

**采样策略**：移动类日志默认按 1/120 抽样（全量会把缓冲区刷爆——一次拖动十分钟能产生十几万行），按键、滚轮、注入是全量。需要临时看全量时，把代码里对应位置的抽样条件去掉（`input_manager.cpp` 等处有 3 个 `%120==0` 的判断，注释里标注了位置）。

**一条经验**：死锁类问题，"卡死前的最后一条日志"就是最后一跳——问题就在它后面那一步。
