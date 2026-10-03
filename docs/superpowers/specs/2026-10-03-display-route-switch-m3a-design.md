# 显示路线参数化开关（winehua.displayRoute）+ 可交互台架 —— M3a 设计

> 状态：设计提案（待实现计划）
> 日期：2026-10-03
> 上游：[display-route-x11-wlroots 设计](2026-09-27-display-route-x11-wlroots-design.md) 的 M3 前置节。
> 本文范围 = 该 spec 中「Pad 模式 = 单 output 虚拟桌面」的第一块增量：把 X 路线从
> smoke 专属 demo 升级为可人工交互的台架，并建立 wayland/x11 的参数化切换。
> 证据纪律同上游 spec：论断带 file:line，未实证处显式标注。

## 1. 背景与目标

X 路线（winex11 + Xwayland + wlroots）M0–M2 已收尾：链路、输入链、零拷贝 present、
guest 帧通道、判定设施全通（出口证据见 known-issues §2 各节）。但当前它只被
「测试客户端 + notepad 载体」验证过，且唯一入口是 smoke 面板按钮 + 240px 预览
XComponent（`SmokeDevPanel.ets:61-73`）——无法人工操作真实程序。

重构的出口判据是「触发 9/27 重写的那批长尾程序能跑」，而原则 #21 要求真实输入
与画面必须人工过一遍 → **台架必须先达到「人能操作」**。

**本设计交付**：
1. app 级路线参数 `winehua.displayRoute`（默认 wayland，零行为差异；x11 经
   Ability 参数 / smoke CLI 启用，冷启动生效）
2. 路线 = x11 时：合成器绑**桌面 surface**（全尺寸）+ 真实触摸/键盘输入 +
   随程序启动的生命周期
3. 输出尺寸参数化（§1.4 的六处 800×600 收敛为一处来源）
4. smoke `--display-route` CLI + 与 job 声明路线的一致性检查

**明确不做**（防范围膨胀，全部留给后续里程碑）：fusion 每窗一系统窗、IME、
剪贴板、任务栏、游戏指针锁定、运行中动态 resize、自动路线回退、设置 UI。

## 2. 裁决机制（单点、同 desktopMode 语义）

### 2.1 先例与复用

`winehua.desktopMode` 已确立整套语义，逐条对照复用：

| 环节 | desktopMode 现状 | displayRoute 做法 |
|---|---|---|
| 解析 | `EntryAbility.ets:69-72` 读 want 参数 → Service 单例 | 同构：`EntryAbility` 读 `winehua.displayRoute` → `DisplayRouteService.setOverrideMode` |
| 单例理由 | AppStorage 跨 Ability 读取不可靠（`EntryAbility.ets:67-68` 注释） | 同理由，同结构（`DesktopModeService.ets:20-27`） |
| 冷启动 | 应用在跑须先 force-stop（`smoke.py:467-473`） | 同款约束、同款报错文案 |
| aa start | `--ps winehua.desktopMode <mode>`（`smoke.py:614-616`） | `--ps winehua.displayRoute x11` |
| CLI | `--desktop-mode {virtual,fusion}`（`smoke.py:1233-1234`） | `--display-route {wayland,x11}`，默认不传 |

### 2.2 决策

- **值域**：`wayland`（缺省）/ `x11`。其它值按缺省处理并 WARN（不 die——参数拼错
  不该挡住正常启动，但要留痕）。
- **不持久化**：与 desktopMode 不同（它有持久化，因为是产品特性）。路线是实验
  通道，**每次冷启动必须显式带参**——避免「某次实验后 app 永远停在 x11」的僵尸
  状态；引擎「重启 Wine」等应用内会话操作不重置裁决（服务单例存续期间有效）。
- **单点裁决，两侧同源**：`DisplayRouteService` 是唯一权威。host 侧（合成器
  启动/surface 分发/输入分发）与 guest 侧（wine 子进程 env 三键）都从它取值。
  禁止任何代码路径绕过服务自行判断 route。
- ** guest 侧 env 仍保留既有机制**：smoke job 的 `WINEHUA_DISPLAY_ROUTE`
  （套件钉档位用，F7 判定依赖它）不动；app 参数缺席时 guest 侧行为一字不变。

## 3. Host 侧三件交付物

### 3.1 Surface 分发（一处分支，两条路各归其位）

桌面 XComponent 的绑定回调（`DesktopWindow.ets:9-14`
`DesktopController.onSurfaceCreated` → `testNapi.createRenderer(rootId, surfaceId)`）
是该 surface 的唯一生产点。改造为**唯一一处** route 分支：

- route=wayland：现状不变（`testNapi.createRenderer`）。
- route=x11：调既有入口 `smokeNapi.smokeDisplayRoute(surfaceId)`（签名已支持
  传 surfaceId，`SmokeRunner.ets:167` 同款），合成器获得全尺寸桌面 surface。

**自动化台架不搬家**：smoke displayroute 套件继续用 smoke 面板 preview surface
——§2.15 背景探测器、presented-route 判定、帧证据链都是在这条链上校准的，
证据连续性优先；人工台架与自动台架共用同一个合成器内核，只是 surface 不同。

surface 销毁（用户关桌面窗）→ `onSurfaceDestroyed` → 走合成器既有收尾路径
（`display_compositor.cpp` fail 分支已实现逆序销毁 + `g_started` 复位重试语义）；
重开窗口 = 重走 start（retrigger 通道已有）。

### 3.2 尺寸参数化（六处收敛为一处来源）

现 800×600 写死于 `ohos_output.c:1067`（query）、`:1255`（headless_add_output）、
`:1269`（custom_mode）、`:1299`（背景 rect）、`:1312`（SET_BUFFER_GEOMETRY）、
`:1368`（日志）。收敛为 `wl_ohos_output_chain_start` 增加的 `w/h` 入参（唯一来源 =
绑定时刻的 surface 尺寸），内部全部取该值。`display_input.c:282` 的 `fw/fh`
同源传递（注入脚本坐标本已是归一化浮点，`:429-452`，不受影响）。

**动态 resize 不做**：运行中 surface 尺寸变化只记日志（后续里程碑再考虑
swapchain 重建——不为台架引入这块风险面）。全幅重绘成本在 1080p/60Hz 下
~7ms（known-issues §2.15 实测推算），参数化后无需动 damage 重构。

### 3.3 输入分发（一处分支，两种模型互不渗透）

两条路线的输入模型本来就不同，不做统一抽象：

- wayland：逐窗模型，`testNapi.sendPointerEvent(windowId, action, px, py, btn)`
  （`WinePopup.ets:113-247` 同款）——不动。
- x11：合成器级归一化坐标，`wl_ohos_input_post_motion(nx, ny, phase)` /
  `wl_ohos_input_post_key(keycode, press)`（`display_input.h`，任意线程安全，
  内部投递队列转循环线程）——**M1 已建好，缺的只是真实事件源**。

分发点唯一：桌面 surface 的触摸/按键处理层。route=x11 时把触摸事件换算为
`(px/surfaceW, py/surfaceH)` 调 `post_motion`（phase 映射：按下=enter、移动=
motion、抬起=leave），物理键盘 `onKey` → evdev 键码 → `post_key`。
route=wayland 时走既有路径。**禁止在第二处出现 route 判断**（否则将来排查输入
问题要查两条分发链）。

需要的新 NAPI：仅输入转发（若 smoke_napi 尚未导出 post_motion/post_key 的
通用版；实现计划第一步核对，已有的话零新增）。

## 4. Guest 侧（env 三键，一个汇集点）

route=x11 时 wine 子进程需要：`WINEHUA_DISPLAY_ROUTE=x11`、`DISPLAY=:0`、
**删除** `WAYLAND_DISPLAY`（互斥语义，`wine_child.cpp:492-498` 的既有分支就是
消费方，守卫 `:504-508` 保留为最后防线）。

**注入点 = 子进程 env 的汇集处，一个工具函数两处调用**：
`stampDisplayRoute(env, route)` —— smoke 路径（`__env`，`SmokeRunner.ets:259-272`
构造的 environment 表）与产品路径（引擎会话 env 构造处）各调一次。函数是唯一
实现，route 取自 `DisplayRouteService`；实现计划第一步钉出两条路径现有的 env
汇集函数名并接上，不设第三入口。

NCP 子进程不继承 app 环境（`display_compositor.cpp:240` 注释实证），所以必须
显式进 env 表，不能靠 setenv 继承。

## 5. smoke 集成

- `smoke.py run --display-route {wayland,x11}`：冷启动携带
  `--ps winehua.displayRoute <v>`（抄 `:614-616`），应用在跑时同款 force-stop
  报错（抄 `:467-473`）。
- **一致性检查（F7 同款纪律）**：`--display-route x11` 时，选中测试必须显式声明
  `WINEHUA_DISPLAY_ROUTE=x11`，否则 run 前 die——理由：app 参数会经 §4 的汇集点
  改写 guest env，无声明的套件（如 core）会被静默拖进 x11 跑，判定层期望路线
  随之失真；声明可以钉 guest 档位，改不了 host 裁决，两处各说各话 = 排查地狱。
- 套件定义不动：core 仍默认跑 wayland；displayroute 套件自带 x11 env 照旧。

## 6. 风险与防坑

1. **默认态零差异是硬门**：参数缺席时，不新建任何对象、不走任何新分支
   （服务取值是一次内存读）；验收含「无参冷启动与当前 HEAD 行为逐项一致」。
2. **单例初始化次序**：`DisplayRouteService` 在 EntryAbility onCreate 解析，
   DesktopWindow/WineEnvService 等消费者全部经单例取值（跨 Ability 读 AppStorage
   不可靠的坑，desktopMode 已踩过并留下结构答案）。
3. **合成器单例与桌面生命周期**：`g_started` + retrigger 机制已有；x11 下桌面窗
   关开循环必须验证「关→收尾干净→开→重新绑定新 surfaceId」（surfaceId 每次创建
   都会变，旧值不可复用）。
4. **输入线程纪律**：post_* 任意线程安全是 M1 实证过的；分发层不得缓存 surface
   尺寸的过期值（onSurfaceChanged 时更新，未启动时丢弃事件）。
5. **不做的东西写在这里防蔓延**：见 §1 末尾清单。任何「顺手」加进去的功能都
   违反本轮范围。

## 7. 验收判据（先于实现确定）

1. **零差异门**：无参冷启动 → core 套件 4/4 PASS，行为与变更前一致，无新增日志。
2. **x11 台架**：`--display-route x11` 冷启动打开桌面 → 合成器绑桌面 surface，
   输出尺寸=surface 尺寸（rate 行与 output chain 日志为证），测试客户端全尺寸
   可见。
3. **人工交互（原则 #21）**：触摸拖动 xclient 窗口跟手、物理键盘输入到 notepad
   可见回显、双窗命中正确（点谁谁有焦点）。
4. **真实程序落位**：route=x11 下经 C:\ 启动任意 exe → presented-route 判定
   PASS（证据自动归档 x11）。
5. **A/B 对账**：同一 displayroute job 两路线各跑一轮，判定均 PASS 且证据按
   route 区分。
6. **毁灭性重建**（复现原则 #4）：`rm -rf build/` 从零完整构建 + 上述 1/2/5 全绿。

## 8. 实现计划衔接

spec 批准后按 writing-plans 出实现计划，任务切分预估：
T1 参数服务与解析 → T2 尺寸参数化 → T3 surface 分发与生命周期 → T4 输入接线
→ T5 guest env 汇集点 → T6 smoke CLI 与一致性检查 → T7 验收跑批（§7 全项）。
每个任务带可验证判据，T7 的毁灭性重建演练压轴。
