# 术语表

> 适用场景：遇到不认识的词时查这里。技术名词保留原文写法（如 DXVK、venus），通用说法用中文。
> 最后核实：2026-09-22

## 图形与渲染

**档位**——选择用哪套图形后端。项目里"档位"有两种用法，看上下文区分：(1) 渲染档位，在设置里切换，取值为 wined3d、dxvk_legacy、dxvk_modern_2_6、vkd3d_limited_500k；(2) box64 的兼容档位（stability、conservative、intermediate、performance），是动态翻译器的参数预设，跟渲染无关。

**wined3d**——Wine 自带的 Direct3D 实现（把 D3D 调用转成 OpenGL）。项目里作为回退档使用。

**dxvk_legacy**——定制的 DXVK 1.10.3（把 D3D9/10/11 调用转成 Vulkan），给 Vulkan 能力不足的设备用。

**dxvk_modern_2_6**——定制的 DXVK 2.6.2，需要 Vulkan 1.3 和 robustness2 扩展。Maleoon 920 支持，910 不支持。

**vkd3d_limited_500k**——vkd3d-proton 2.6 的 Direct3D 12 档，descriptor 堆上限设为 50 万条（上游要求 100 万）。这一档还要另外选 DXVK 档，因为 D3D11 仍由 DXVK 负责。

各档位的适用设备和选择规则见 [decisions/0002-d3d-backend-profiles.md](decisions/0002-d3d-backend-profiles.md)。

**venus**——把 Vulkan 命令虚拟化后传给宿主 GPU 的通道（guest 侧是 mesa 的 venus 实现，宿主侧是 virglrenderer 的 venus 实现）。DXVK 和 vkd3d 走这条通道。

**virgl**——把 OpenGL 命令虚拟化后传给宿主 GPU 的通道。OpenGL 程序走这条。

**vtest**——guest 与宿主渲染器之间的命令传输协议，由 virglrenderer 自带的测试服务器（virgl_test_server）承载。

**virpipe**——guest 侧 mesa 里负责通过 vtest socket 发送命令的部分。

**vn\_ / vkr\_**——venus 协议两端的文件命名前缀：`vn_*` 是 guest 侧（在 mesa 仓库里），`vkr_*` 是宿主侧（在 virglrenderer 仓库里）。

**shadow**——宿主侧的内存同步方案。设备不支持 dma-buf 导出时（如 Maleoon），用匿名文件做一份"影子内存"，在 guest 和宿主之间拷贝同步。

**dirty-ring**——内存同步的一种档位（配合 shadow 使用）。vkd3d 只能用这一档：它没有 flush 发布点，用 explicit 档会黑屏。

**zero-copy（零拷贝）**——virgl 生效时，画面不经过像素拷贝，宿主把渲染缓冲直接接进渲染链路，提交时只传资源编号和窗口归属。

**直传**——全屏画面的一条快路径：跳过 CPU 合成，把原始帧直接交给 GPU 缩放上屏。

**ARM64X**——一种 PE 文件格式，同一个文件里含 aarch64 和 arm64ec 两套镜像。arm64 方案下 Wine 内置 DLL 和 DXVK 系列 DLL 用这个格式产出。

**ARM64EC**——ARM64 上运行 x86_64 代码的兼容层。单独的 ARM64EC DLL 会被 Wine 加载器拒绝，只能作为 ARM64X 的一部分存在。vkd3d 的 d3d12 最终保持 x86_64 单架构。

**受管运行时（managed runtime）**——构建产出的 vkd3d/dxvk 目录树，通过环境变量让它优先于应用自带的 DLL 加载。

## 合成与窗口

**compositor（合成器）**——项目自研的 Wayland 合成器，跑在应用主进程里。负责把各窗口的画面合成上屏、决定窗口叠放顺序、裁决输入事件发给哪个窗口。约一万行原生代码。

**toplevel**——Wayland 的顶层窗口概念。Wine 的每个窗口对应一个 toplevel。

**subsurface**——依附于父表面的子表面，用于窗口客户区、菜单、浮层这些内容。

**桌面根窗口（root）**——Wine 的 `#32769` 窗口，被标记为 `explorer.exe.desktop-shell`。合成器识别它作为桌面合成的基底，其他窗口合入它的帧。

**ghost 窗口**——空标题的辅助 desktop-shell 窗口，尺寸与真桌面完全相同。因为尺寸分不出来，识别真桌面只能用标题。

**虚拟桌面**——所有 Wine 窗口合成到一个全屏画面里。平板默认用这个形态，手机固定用它。

**多窗口模式 / 融合模式**——每个 Wine 窗口对应一个系统窗口：PC 上是独立的 UIAbility 窗口，平板上是主窗口下的子窗口（subWindow）。两种承载在 Wine 和原生侧完全相同，差别只在界面层的承载方式。

**内嵌客户区 / 越界浮层**——多窗口模式下区分两种 subsurface 的判据。内嵌客户区（不接收输入、不设 viewport）合进父窗口的帧，输入穿透给父窗口；越界浮层（菜单、未受管的子窗口）保留独立系统窗口，可以越出窗口边界。

**MW-\* 日志标签**——合成器各模块的日志前缀（协议层统一用 `WL_Server` 标签）：`MW-RNDR` 渲染循环、`MW-TAKE` 取帧合成、`MW-SUBSURF` 子表面、`MW-MOVE` 窗口拖动、`MW-LIMITS` 窗口尺寸限制、`MW-COMMIT` 帧提交。`MW-RAISE`（窗口置顶诊断）目前只记录在排查笔记里，代码中还没落地。

## 输入

**相对指针模式**——游戏隐藏并锁定光标时，鼠标事件改传相对位移（用 `zwp_relative_pointer_v1` 协议）。是否进入这个模式由 Wine 侧判断。

**触控板模式**——把触摸屏当触控板用（光标在哪就点哪），与直接触控（指哪点哪）互斥。是虚拟鼠标模式里的一个开关。

**光标锁定**——Wine 请求锁定光标时，宿主冻结光标位置并隐藏它。

**光标门禁**——宿主侧判断"能不能隐藏并冻结光标"的条件（按显示形态和是不是桌面根窗口判断）。隐藏和冻结必须一起判断——只拦隐藏会得到"光标还在但动不了"的状态。

**WHGP**——WineHua 手柄协议。宿主的手柄状态经 Unix socket 传给 winebus，Wine 的震动请求回传给宿主。两端各有一份协议头文件，必须同步修改。

## 进程与构建

**三方案**——项目同时支持三种运行方式：x86_64 原生（PC）、box64 转译（arm64 设备上跑 x86_64 的 Wine）、arm64 原生（Wine 和转译层都是 arm64）。构建目录按架构隔离。

**box64**——x86_64 到 ARM64 的动态翻译器。box64 方案里编译成 `box64.so`，由 wine_child 进程加载，在同一个进程里执行 x86_64 的 Wine。

**FEX**——另一套 x86_64 到 ARM64 的翻译器。arm64 原生方案里负责转译 x64 应用（它不能模拟 ELF，所以 guest Vulkan 停用，走宿主原生 Vulkan）。

**NCP**——系统提供的子进程创建方式（`OH_Ability_StartNativeChildProcess`）。限制是子进程里不能再调用 NCP 接口。

**fork**——手机模式的兜底方案：系统 NCP 不可用时，用 POSIX fork 实现同样的功能。

**broker**——进程启动代理，跑在应用主进程里（Unix socket，路径 `.../files/.wine_broker`）。所有 Wine 进程（wineserver、wineboot、explorer、游戏程序、Wine 内部创建的子进程）都通过它启动：子进程把启动参数发回来，由主进程代调系统的创建子进程接口，同时完成环境变量注入。

**Wine prefix**——Wine 的虚拟 Windows 目录（包含 drive_c、注册表等）。项目里位于沙箱的 `.../files/.wine`，支持多个 prefix 切换。

**首启 / 二启**——按 prefix 的磁盘状态分流：prefix 未初始化时走完整的 `wineboot --init`（首启），已初始化时只播种启动事件（二启）。

**热重启**——引擎（wineserver）还活着，但桌面会话没了，此时启动一个新的 explorer 会话连到现有 wineserver。

**Wine 会话**——合成器状态的生命周期单位。状态按"会话"而不是"进程"来管理，这样热重启和冷启动从同一基线开始，不会留下上一轮的残留状态。

**stamp 机制**——Makefile 用 `build/.stamps/` 下的标记文件判断某个构建目标是否需要重跑，避免每次都重新编译。

## 协议

**跨仓库契约**——wine、mesa、virglrenderer 三个仓库之间"两边各写一份、内容必须完全一致"的配合约定，共 10 条（C1 到 C10）。因为仓库之间不能共享头文件，靠魔数、版本号、长度字段自检。完整清单见 [architecture/contracts.md](architecture/contracts.md)。

**winehua_toplevel 协议**——自定义的 Wayland 协议，只有一个请求（`set_modal`），用来表达 Win32 的模态对话框关系——xdg-shell 协议里没有这个概念。表单见 `entry/src/main/cpp/protocols/winehua-toplevel.xml`。

**surfaceKey**——窗口的复合标识（guest 进程号加上 surface id），用于 zero-copy 标记文件和按窗口管理渲染资源。

## 自动化测试

**smoke**——自动化回归测试设施。用真实的 Windows 程序验证整条链路能否跑通。

**套件（suite）**——测试用例的分组，声明跑哪些用例、用什么参数。

**用例（case）**——一个目录一个测试，用 `test.json` 定义。

**载荷（payload）**——测试程序的集合（可执行文件、套件定义、清单），开发环境由脚本推送到设备，发布环境打进安装包。

**判定器（check）**——判断测试是否通过的纯函数。判定和执行分开，可以对历史结果重新判定。

**门禁（gate）**——发布前的验证关卡（注意与"光标门禁"区分，后者讲的是光标隐藏条件）。

## 其他

**phone 模式**——系统 NCP 不可用时的一种隔离形态：用 fork 替代 NCP，virgl 宿主不单独建进程，改为主进程内加载。合成器不感知这个模式。
