# 启动器 UI 设计与功能演进路线

本文档规划启动器（EntryAbility 主界面）的交互设计、PC/Pad 多端适配策略，
以及提升实用性的新功能候选与优先级。不涉及 compositor / wine 内部逻辑。

## 1. 现状诊断

当前主界面 `entry/src/main/ets/pages/Index.ets` 本质是开发调试台：

- 首页 = `drive_c` 文件浏览器，用户需自行翻目录找 exe（工程师视角）
- 初始化/重置/重启 Wine、进程列表、kill 按钮全部平铺，高危操作无分层
- 解压 249MB wine-data.zip 仅有文字 overlay，无进度（本期明确不做进度条）
- 41 处颜色硬编码 hex，未走语义资源；深浅色、多 DPI 不可控
- PC（2in1）与 Pad（tablet）共用同一布局：300vp 固定侧栏，Pad 触摸目标偏小，
  PC 窗口缩窄后不自适应

设备分流现状（保留，不属于启动器 UI 范畴）：

- `WineWindowManager.ets` 按 `deviceInfo.deviceType` 分流：
  tablet/phone → 桌面合成模式（DesktopAbility，单 XComponent 全屏）；
  2in1 → 托管窗口模式（WineWindowAbility multiton，每 Wine 窗口 = 系统窗口）

## 2. 设计原则

**一套信息架构，两种壳。** 统一性来自统一的数据模型与组件库，外层容器按断点
自适应（HarmonyOS "一次开发、多端部署"范式：xs 320 / sm 600 / md 840 / lg，
mediaquery 驱动），而不是按 deviceType 写两套 UI。设备形态分叉只保留在窗口
宿主层（WineWindowManager），不进入启动器 UI。

## 3. 现有界面改进（四步，每步独立可构建验证）

### Step 1：视觉基础统一（纯资源层，零行为变化）

- `resources/base/element/color.json` 定义语义色：window_bg / panel_bg /
  header_bg / card_bg / overlay_bg / divider / control_bg / text_primary /
  text_secondary / text_tertiary / text_disabled / accent / accent_soft /
  state_ok / state_warn / state_info / danger / action_primary /
  action_danger / action_warning / shadow
- dark 目录放同值副本（EntryAbility 强制 COLOR_MODE_DARK；未来浅色主题只改 base）
- Index.ets 硬编码色全部替换为 `$r('app.color.*')`
- 仅两处有意的微小归并：overlay 标题 #f4f7fb→text_primary，
  正文 #aeb8c6→text_secondary
- 窗口背景透明 `#00000000`、桌面黑底 `#000` 等是合成协议的功能性颜色，
  不做资源化

### Step 2：数据层抽离（UI 原样，行为不变）

```
ets/service/
  WineEnvService.ets      // state 回调、prefixReady、init/reset/restart
  ProcessService.ets      // 进程列表刷新、kill
  AppLibraryService.ets   // 用户收藏条目（手动添加），preferences 持久化
```

`testNapi.setStateCallback` 从页面搬入 service，页面只订阅状态。

### Step 3：IA 重构——应用库首页 + 组件拆分

```
ets/components/
  AppLibraryView.ets   // 新首页：exe 卡片/列表，点击启动
  FileBrowserView.ets  // C:\ 浏览器降级为 Tab，exe 行内"加入应用库"
  TaskListView.ets     // 运行中进程列表：仅"结束"操作；启动入口只属于应用库
  EnvPanel.ets         // Wine 状态 + 重启/重置（重置加二次确认）
ets/pages/Index.ets    // 瘦壳：Tabs(应用库/任务/文件/环境) + 四组件
```

四个 Tab 内容 PC/Pad 完全一致——统一性体现在这一层。

应用库条目来源（用户已裁定）：

- **M1 仅手动添加**：文件浏览 Tab 的 exe 行内"加入应用库"，PC 拖拽入库
  （Step 4），Pad 长按菜单（Step 4）。首启应用库为空，空态必须给出引导
  （"去文件页把 exe 加入应用库"）
- **M3 增加 lnk 扫描**：解析 winemenubuilder 写入 Start Menu 的 .lnk，
  按目标路径与手动条目去重合并，自带规范名称与图标
- **暂缓**：drive_c 自动扫描（噪音过滤成本高）、安装包运行后的
  "发现新程序"入库钩子——两项均暂不实现
- **已否决（用户裁定，勿再提起）**：任何形式的目录扫描，含 Z: 映射
  Download 专属子目录扫描（曾被作为低成本折中形态提出，已否决）。
  应用库 = 纯手动入库；lnk 扫描属 M3 既有项，不在否决范围

### Step 3.5：Pad 单 Ability 化（桌面与启动器合并）

问题：Pad 现状是 EntryAbility（启动器）+ DesktopAbility（桌面）两个 Ability，
多任务列表出现两个图标，切换割裂；且杀掉启动器后桌面成孤儿。

方案：DesktopAbility 承载的只是一个全屏 XComponent（wine_desktop_xc）+ 输入
注入管线，XComponent 只认 surfaceId，不依赖独立窗口，无必要独占 Ability。
合并为 EntryAbility 单 Ability，主页面改三层 Stack：

```
Layer 2: 悬浮 Home 球（仅桌面运行中且启动器隐藏时可见，可拖动贴边）
Layer 1: LauncherView overlay（Step 3 的组件化启动器，整页显示/隐藏）
Layer 0: DesktopLayer（DesktopWindow.ets 原样抽组件，桌面运行期间常驻挂载）
```

- desktop_root 出现 → 自动显示桌面层（保持现状自动进桌面行为）；
  desktop_root 消失 → 自动回到启动器
- 切回入口：Home 球（主）+ 自动规则 + onBackPress 接管（启动器可见时
  返回键 = 回桌面，不退出 app）；不依赖系统边缘手势，避免与游戏边缘触摸冲突
- Home 球的演进方向：可收纳 pill，
  含 Home / 键盘开关 / 输入模式 / 隐藏——键盘唤起与输入模式是在局高频
  操作而非设置页低频项；M4 触摸映射的布局编辑器也挂在此工具条。
  M1 先做单球，结构上预留 pill 形态
- 输入隔离天然成立：overlay 可见时触摸/键盘焦点全落在 ArkUI，
  XComponent 被遮盖收不到事件
- WineWindowManager.startDesktopAbility 改为状态事件由根页面订阅；
  module.json5 删除 DesktopAbility；Pad 回归单 mission，
  杀 mission = 杀整个 app + wine，生命周期一致
- PC（2in1）不受影响：托管窗口仍走 WineWindowAbility multiton
- 遗留优化：启动器全不透明遮盖桌面层时 XComponent 遮挡仍渲染，白耗 GPU，
  后续可做遮挡时 frame callback 节流（不阻塞本方案）
- 否掉方案：隐藏 mission（第三方无可靠 API 且切回入口未解决）；
  启动器改子窗口（引入两个真实窗口的焦点/输入法拉扯，popup 已踩坑）

### Step 4：断点 + 分形态优化

- 引入 BreakpointSystem（mediaquery）
- PC（2in1）：md 以下底部 TabBar 单栏，md 以上 Navigation 左导航右内容；
  exe 卡片 bindContextMenu 右键菜单；支持拖拽 .exe 入库（onDrop）；hover 态
- Pad（tablet）：固定侧边 TabBar，触摸目标 ≥48vp；应用库 Grid 大卡栅格；
  长按呼出同一 bindContextMenu；"任务"页展示运行中窗口

### 明确不做（本期）

- 解压进度条（用户指示暂缓）
- phone 形态单独优化（deviceTypes 含 phone，维持可用即可）
- 桌面模式任务栏与启动器互唤（涉及 compositor，单独立项）

## 4. 新功能候选（按实用价值排序）

### 第一梯队：直接决定"能不能用"

1. **触摸输入映射（Pad 命脉）**：可拖放虚拟按钮/摇杆 → 键盘键码/鼠标动作
   （ExaGear/Winlator input-controls 模式）。合成器已有键鼠注入通路
   （MouseMap/KeyMap），新功能是"虚拟控件→注入 API"映射层 + 布局编辑器。
   成本中高，建议单独立项做透
2. **exe 图标提取 + 快捷方式扫描**：winemenubuilder 已提取图标到 prefix
   `~/.local/share/icons`，可扫描复用；裸 exe 解析 PE .rsrc 取图标；
   扫描 Start Menu `.lnk` 自动入库。成本中
3. **一键导入/安装（暂缓）**：从 Download 选 exe 直接运行（Z: 已映射），
   安装完成后提示"发现新程序，加入应用库？"。成本低。
   用户裁定暂缓：其"安装后入库钩子"依赖扫描 diff，与自动扫描同属暂缓项
4. **应用内日志查看**：展示 wine stderr / compositor 日志，支持复制/导出，
   排障不再依赖 hdc。成本低

### 第二梯队：兼容性覆盖率

5. **每应用配置（profile）**：启动参数、环境变量、渲染后端、缩放系数按应用
   存储。重点项：**wine 虚拟桌面模式**（`explorer /desktop=name,1600x900`），
   老游戏兼容性显著更好且规避窗口管理问题，仅是启动参数，成本极低
6. **多 prefix / prefix 备份**：应用隔离；备份 prefix = 备份游戏存档。
   成本中（native 侧 prefix 路径需参数化）

### 第三梯队：体验润色

7. 游戏内 overlay：FPS 显示（virgl 调优刚需）、屏幕常亮
8. 虚拟键盘唤起（Wine 程序文本输入目前无解）
9. 分享接收：文件管理器 exe "用本应用打开"

## 5. 里程碑

- **M1**：Step 1-4 现有界面重构（含 3.5 Pad 单 Ability 化；仅 Step 3.5
  涉及 WineWindowManager/module.json5 的 ArkTS 改动，不碰 native）
- **M2**：新功能 4（日志页）+ 5（每应用配置/虚拟桌面）——低成本高确定性；
  虚拟桌面模式可能绕过一批窗口/全屏难题（一键导入已暂缓，移出 M2）
- **M3**：新功能 2（图标 + lnk 扫描）——应用库成型
- **M4**：新功能 1（触摸输入映射）——Pad 大工程，单独立项
- 第二/三梯队其余项视反馈插入

## 6. 开发路线（执行顺序与验证门槛）

通则：

- 每步一个独立 commit，可单独 revert；提交按显式文件清单 add，
  不用 `git add -A`（工作区常驻用户的 wine/构建配置改动，不裹挟）
- 每步结束：`make NATIVE_ARCH=arm64-v8a` 构建 + Pad 安装回归；
  仅 Step 4 追加 `x86_64` PC 模拟器回归
- 建议从 feature/split-wayland-server 切 `feature/launcher-ui` 分支承载 M1
- M1 全程不碰 native（cpp/wine）；Step 3.5 只动 ArkTS 与 module.json5

### Step 0：准备

- 确认用户侧改动已收尾 → `stash pop` 取回 Step 1 语义色定义
- 当前 HEAD 基线构建一次，确认可装可跑

### Step 1：语义色（零风险）

- 改动：base/dark color.json（stash 中取回）+ Index.ets 41 处 `$r` 替换
- 验证：Pad 启动器视觉与改前对照无差异
- commit：`refactor(ui): 启动器颜色语义资源化`

### Step 2：数据层抽离（行为不变）

- 改动：新增 WineEnvService / ProcessService / AppLibraryService；
  Index.ets 改为订阅 service
- 验证：初始化 → 启动 exe → 进程列表刷新 → kill → 重置，全链路一致
- commit：`refactor(ui): 启动器数据层抽 service`

### Step 3：IA 重构（首个用户可见变化）

- 改动：AppLibraryView / FileBrowserView / TaskListView / EnvPanel 四组件；
  Index.ets 变 Tabs 瘦壳；重置加二次确认；应用库手动入库 + 空态引导
- 验证：四 Tab 功能完整；启动/kill/重置回归；依赖 Step 2 的 service
- commit：`feat(ui): 应用库首页与四 Tab 信息架构`

### Step 3.5：Pad 单 Ability 化（M1 风险最高，独立成步）

- 前置：Step 3 的 LauncherView 组件（启动器必须已可从页面剥出）
- 改动：DesktopWindow.ets → components/DesktopLayer.ets（原样搬运）；
  根页面三层 Stack（DesktopLayer / LauncherView / HomeBall）；
  WineWindowManager.startDesktopAbility → 状态事件；EntryAbility 沉浸切换；
  module.json5 删 DesktopAbility；删 DesktopAbility.ets
- 验证（Pad 重回归）：冷启动 → 启动器 → explorer 桌面自动出现 →
  Home 球切回 → 启动游戏 → 全屏 → 返回手势 → popup/异形窗口 →
  杀 mission 整体退出；PC smoke（确认 module.json5 变更不影响托管窗口）
- commit：`refactor(ui): Pad 桌面与启动器合并为单 Ability`
- 回滚预案：整步单 commit，revert 即恢复双 Ability

### Step 4：断点 + 分形态

- 改动：BreakpointSystem；PC Navigation/右键/拖拽/hover；
  Pad 侧 Tab/大卡栅格/长按/≥48vp
- 验证：Pad + PC x86_64 模拟器双端回归（PC 窗口拉宽拉窄）
- commit：`feat(ui): 断点自适应与 PC/Pad 分形态交互`

### M2 及以后

M1 收口后按里程碑顺序展开：应用内日志 → 每应用配置
（含虚拟桌面模式）→ 图标/lnk 扫描；触摸输入映射单独出方案立项。
（一键导入、drive_c 自动扫描均已暂缓。）
每项开工前单独出细化设计，不在本文档展开。
