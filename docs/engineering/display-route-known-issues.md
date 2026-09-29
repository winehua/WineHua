# 显示路线已知问题与后续处置清单

> 来源：M1 收尾 whole-branch 评审（2026-09-29，范围 60e26c7..7237771）的
> Minor 缓办项 + 技术悬案 + 操作协议。执行 ledger（git-ignored 工作区）
> 已按流程删除，**本文件是其耐久承接地**。
> 用法：M2 开工时逐条过一遍（§1 是开工前必修/顺手修清单，§2 是排查
> 悬案入口，§3 是必须遵守的操作协议）；修掉一条就删一条，删完本文件
> 即可整体移除。
> 设计 spec：`docs/superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md`
> （§6.3 = M1 实测结论）；M1 验收：`docs/superpowers/specs/2026-09-29-display-route-m1-acceptance.md`。

## 1. 代码埋雷（按触发时机排序）

### 1.1 munmap 长度与 mmap 不一致（M2 动输出路径前必修）

`ohos_output.c` `HandleOutputCommit` 路径 G（无 LockBuffer 的老设备回退）：
映射用 `bytes = h->size`（BufferHandle 自报总量），解除用
`munmap(dst, (size_t)ht * dst_stride)`（行数×行距自算）。两者在当前
800×600 固定输出下恰好相等所以无症状；只要 buffer 带尾部填充或高度被
钳制，munmap 长度就对不上 mmap——严格说踩在未定义行为边上。
**修法**：munmap 复用同一个 `bytes` 变量，半小时。
**同区另两处顺手修**（同一函数、同一时机）：

- stride 不一致时整帧静默跳过（`dst_stride == src_stride` 不满足即丢帧，
  无日志无计数）——M2 换零拷贝/分辨率后 stride 可能分叉，届时表现为
  "偶发掉帧且零证据"。修法：else 分支加限频日志 + 计数。
- `OH_NativeBuffer_Map` 源映射失败早退不归还 window buffer（路径 G 已
  RequestBuffer 拿到的槽位泄漏，连续几次 RequestBuffer 即饿死）。修法：
  失败分支 FlushBuffer 或取消归还。

### 1.2 合成器启动失败路径资源泄漏 + retrigger 管道无锁赋值（M2 结构整理时收）

`display_compositor.cpp` 启动链（wl_display → loop → renderer → … →
Xwayland）任一环失败即线程 return，已建资源不销毁；`g_retrigger_pipe`
由 worker 线程赋值、主线程读判空，无锁（当前时序安全：赋值发生在
`g_started=true` 锁内之后，无同步保证）。同区：retrigger `write()` 返回
值未查、`ohos_buffer.h` 重复 `#pragma once`、`xclient_child.cpp` 解析
`appPid` 后未消费。**修法**：线程体改统一 cleanup 收尾 + 全局管道变量
收进锁内，一并收掉杂项。触发时机 = displayroute 变常态路径后启动失败
从"开发期偶见"变"用户可触发"。

### 1.3 host-ext 打包 glob 漏两位数 SONAME（下次动 host-ext 依赖时修）

`scripts/assemble.sh:47` `*.so.[0-9]` 只匹配一位版本号——`libx.so.10`
存在、构建成功、打包静默漏掉，设备端 dlopen 失败且病灶离现象很远。
当前闭包全是位数版本（libX11.so.6 等）无症状。**修法**：放宽为
`*.so.[0-9]*`，改前先核对该目录实际文件清单确认不会拷入不该拷的。

### 1.4 800×600 三处独立写死（随 M2 分辨率参数化一起收敛）

`ohos_output.c`（输出实际尺寸）、`display_input.c`（注入脚本几何分母 +
xclient_child 窗口摆位耦合）、`SmokeDevPanel.ets`（4:3 aspectRatio）。
注释已互相点名但仍是漂移起点：改分辨率漏一处即"注入整体偏移"或"比例
失配"，且三层分散难一次看全。**修法**：随 M2 PC 模式/多 output 的分辨
率参数化设计单一来源（ohos_output 导出查询，ETS 侧从配置/ready 标记
读），不要现在单独修——M2 改分辨率还得再动一次。

### 1.5 单槽 pending key + 孤儿 release（与 §2.1 绑定排查，不单独修）

`display_input.c`：settle 窗口（焦点切换后 40ms）内的待发键只有**一个**
槽位（`g_pending_key`），第二个键覆盖第一个（第一个永久丢）；press 记
入 `g_held_keycode` 后若按键因失败路径未发出（如 settle 期焦点 surface
销毁、DeliverPendingKey 无焦点直接 return），release 定时器仍会发出
无配对的 release。**处置**：不凭空加固——按原则 14 先拿可复现时序，
与 §2.1「最后一键」排查绑定；修的时候把两处一起治（队列化 pending、
release 与 press 配对所有权）。

## 2. 技术悬案（M2 排查入口）

### 2.1 「最后一键」间歇不入编辑控件

T5 起挂起：脚本末两键 H/I 四轮真机 2 轮全成、1 轮只有 "h"、1 轮全无；
注入侧日志逐字节同形，差异在 wine 进程内部非确定路径。嫌疑：① §1.5
的单槽机制；② caret blink/damage 竞态；③ 模态关闭后 edit 重聚焦时序。
**排查武器已就位**：`smoke/jobs/displayroute-notepad.json` 重放自带
`WINEHUA_WINEDEBUG=+win,+x11drv,+event`，复现时 wine_stderr 有逐键完整
轨迹（T5 当时缺的就是它）——一旦复现，哪一跳吃键有据可查。

### 2.2 两条 GL 路线回读带宽差 16 倍的机制（M2-T3 后改形）

T6 实测：同 guest mesa virpipe、renderer 串相同，wayland-EGL 读回
19.2MB/s vs「X 路线」310.9MB/s。**M2-T3 实测推翻了其中一项前提**：那条
「X 路线」没有走 X——winex11 的 GL 段在本构建里根本没编入（见 2.4），
WGL 落到 win32u 通用 EGL 驱动 + FBO，无 GLX、无 drisw、无 X 窗口参与。
所以 16 倍差不是「drisw 本地 shadow vs vtest 往返」的路线差，而是两条测
量各自的读回管路差。**待查**：两边具体差在哪一段（嫌疑：wayland 侧读回
经 WineHua 呈现/读回机制，FBO 侧走 virgl 自身 transfer）。**顺带作废**：
「X 路线 310MB/s ≈150fps」不能作为 X 路线 GL 能力或性能的依据，R1 的改
判理由不成立（见 2.4）。

### 2.3 引擎冷启 wineboot 偶发 box64 SIGSEGV 崩溃循环

T5 期 t5q 一例：同套二进制 force-stop 重试一次即成；M1 期未复现。位
于引擎启动路径（wine+box64），本分支代码之外。**复现时先保现场**：
hilog 全量落盘 + stderr 只截尾部 ~1MB（t5q 整拉 744MB 失败教训），再
重试恢复。M2 期复发 ≥3 次升级专项（方向：box64 dynarec 对 wineboot 某
代码段的翻译）。

### 2.4 X 路线没有 GL 呈现（M2-T3 实测定位，补栈与否待范围裁决）

M2-T3 真机（r20260930-014632 与带 `+wgl` 复跑 r-t3glx，设备 .5）判据
不成立，定位到**三处构建层缺件**——X 路线的 OpenGL 呈现链当前不存在，
不是「drisw present 有没有 bug」的问题：

| 环节 | 现状 | 证据 |
|---|---|---|
| Xwayland GLX 扩展 | 无（`-Dglx=false -Dglamor=false`，M0 shm-only 决定） | `scripts/build_xwayland.sh:172` |
| guest libGL（GLX 客户端） | 无：guest_gfx 只有 EGL/GLES/gallium + `dri/swrast_dri.so` | 设备 `guest_gfx/lib` 清单 |
| winex11 GL 段 | 未编入：`WINEHUA_ALLOW_X11_NO_GLX=1`，configure 拿不到 `GL/glx.h` → `X11DRV_OpenGLInit` 落 `#else` stub | 运行期 `display_funcs_init Failed to initialize the driver OpenGL functions, status 0xc0000002` |

状态码是判据：stub 返回 `STATUS_NOT_IMPLEMENTED`(0xC0000002)，真实的
libGL 加载失败返回 `STATUS_NOT_SUPPORTED`(0xC00000BB) 并附 ERR 行——实
测是前者，故为编译期缺件，非运行期缺库。

**实际行为**：WGL → win32u 通用 EGL 驱动 → `egldrv_surface_create` 造
**FBO drawable**（`framebuffer_surface`），`framebuffer_surface_swap` 是
**空实现**（`dlls/win32u/opengl.c:407` 直接 `return TRUE`）⇒ 帧进 FBO、
没有任何真 present。实测吻合：5568 帧 @542fps、`WINEHUA_VTEST_FRONT‐
BUFFER_LOG` 全程一次未写（winsys present 从未被调用）、X 窗口零 damage、
截屏里只有合成器背景 + 注入测试窗（Xlib 路径），固帧四象限始终不出现。
**X 窗口 → scene → 输出 → XComponent 这一段是好的**（注入测试窗正常出
图并动），缺口只在 GL 客户端出图这一跳。

**影响面**：X 路线当前只有「X 窗口语义 + 输入」，OpenGL 程序（含 wined3d
走 GL 的 D3D8/9）在 X 路线不出图；Vulkan 侧（venus / DXVK / vkd3d，即
M2-T5/T6）不经这条链，不受影响。**补栈清单**（三件套，缺一不可）：①
Xwayland `-Dglx=true`（连带 host 侧 DRI/swrast 依赖）；② guest mesa 出
`libGL` + `GL/glx.h` 头（同源、guest 架构）；③ 撤 `WINEHUA_ALLOW_X11_
NO_GLX` 重编 wine。**未做**——属栈建设，超出 M2 阶段 A 范围，等裁决。

**补栈时要一并换的判定器**（两处都不适用本场景，不是补栈就能自动绿的）：
① `dx-glx-present` 程序侧 `displayed` 门读 `WINEHUA_DISPLAY_FPS_FILE`，而该
文件由 wayland 路线渲染器写（`entry/src/main/cpp/common/perf_utils.cpp:32`），
X 路线永远缺 → 它是路线外来判据；应按 T3 计划改程序侧出**帧内容 CRC 序列**
（证明客户端在画），出图与否交给主机侧。② `visual:rgba-quadrants` 对**全屏
截图**做四象限，而 displayroute 的出图面是侧栏里的**预览小框**（`SmokeDev-
Panel` 的 XComponent，4:3、约 500×390 物理像素），四象限永远判不出来；主
机侧需要按区域裁剪的视觉判定器（或按框内 CRC 变化判活）。

## 3. 操作协议（必须遵守，违反即隐性故障）

### 3.1 hdc -b 热更后必须 sha256 对账

实测（M1-T6，t6d/t6e/t6f 三轮）：`hdc file send -b` 热更沙箱文件后，
app 挂载视图与 portal 视图**按文件不一致**（同一次 push，manifest 新/
exe 旧字节；host 回拉却都是新的），seed 按 manifest 版本比对被跳过不
重播，旧 exe 一直被执行且无任何报错。根因疑似 OHOS 沙箱 FUSE per-file
缓存（未追到平台根因）。**协议**：任何 `hdc -b` 热更**二进制**后，
`file recv -b` 回拉 + sha256 与本地对账；不一致或涉及新 exe 直接走
`bash scripts/package.sh deploy`（卸载重装，HAP 内置载荷 app 自解压，
写读同进程自洽）。对账实例见 M1-T7 两次推送。

### 3.2 套件回归前必须冷启动

displayroute 会话状态残留会污染后续套件的帧采集（t5v、T8 两度复现：
displayroute 任务后直接跑 core，抓到 app UI+虚拟桌面合成帧而非测试
全屏窗口；生产者帧率正常，仅采集面错位）。**协议**：跑 core/wine-vulkan
等门禁套件前 `aa force-stop` + 冷启动。已写入 spec §6.3。

### 3.3 glx-bridge-probe 不得用作回归门

该 case 设计为"只出数据不下结论"（R1 裁决在 spec 层），结果 JSON 在
GL 严重退化时仍 PASS——唯一 FAIL 方式是协议层失败（进程崩/无结果文
件）。它是测量仪不是门禁：**不得加进任何作为门禁的套件**。M2 若需要
GL 读回带宽门禁，新写带阈值用例（如 `readMBps < 100` 判 FAIL）。

## 4. 前置条件（场景切换才触发）

- **沙箱安全审计**：wlroots/xserver 补丁含三处在桌面 Linux 语境"看起来
  危险"的改动——`-ac`（关 X 访问控制，peercred 豁免沙箱不生效的实测
  解）、shm fchmod 失败容忍（见 §5）、waitpid 移除（NCP 形态收尸归
  属）。补丁注释均有实测依据；当前威胁模型（单应用沙箱内部）可接受。
  **若方案出给第三方设备/多用户场景，必须先做整体沙箱安全审计**，不
  得带 these 补丁直接出包。
- **帧率债**：displayroute 合成 18.6fps（≥25fps 目标按 T3 预案降级，
  瓶颈=BufferQueue copy+flush）。零拷贝 present 重构（M2 入口 ①）是
  唯一出口；T3 分段遥测刻意留在代码里作前后对照基线。

## 5. 文档欠账

- spec §6.2/§6.3 未记 M1-T1 的 shm fchmod EACCES 平台约束（OHOS 沙箱
  对 tmpfs 文件 fchmod 返回 EACCES/errno 13，SELinux setattr 拒绝；桌
  面 Linux fchmod 仍成功、日志静默）。证据在
  `scripts/patches/wlroots-ohos-shm-fchmod-tolerant.patch` 注释内。**下
  次提交碰 spec §6 时顺手补一行**——补丁注释是证据正本，spec 是记忆
  层；没有这一行，将来升级 wlroots 重打补丁时可能有人把它按"错误经验"
  修回致命版。
- `WineEnvService.ets` readTextIfExists 格式化 churn 已入史（教训：格
  式变更不与功能变更同 diff），无可挽回动作。
