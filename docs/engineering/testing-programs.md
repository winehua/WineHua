# 自研 Windows 测试程序体系与详细用例清单（winehua_t_*）

适用场景：为 winehua 栈规划、实现、接入自研 e2e 测试载荷（PE 测试程序）时看这篇。
目标：自有载荷快速回归验证栈行为，替代「每次找别的软件验证」。每个用例都定义了
**自动化判定手段**与**通过/失败判据**——能证明有问题，也能证明无问题。
用例怎么写/挂载见 `docs/engineering/testing-cases.md`。最后核实：2026-09-26。

## 1. 设计原则

1. **设备端只跑不判**：程序自检并把结论写进 result JSON，主机判定器只读归档。
2. **自检优先于观察**：API 往返 > 离屏像素读回 > 开窗截图；截图只供人工复核，
   固定图案类才参与视觉自动判定。
3. **失败继续跑完**：收集域内全部失败再退出。
4. **独立可复现**：不依赖网络/时间/外部文件/用例顺序。
5. **双架构**：x64 + x86（x86 走 BOX32，是独立回归面）。
6. **复用现有通道**：`smoke/tests/<id>/test.json` + `build.sources` mingw 交叉编译
   （模板 `smoke/tests/win32-driver/test.json`），不进 wine 树。

## 2. 自动化检测手段（六类，每个用例标注所用组合）

| 手段 | 机制 | 判定方 |
|---|---|---|
| **R**·result JSON | 程序内 API 断言累积 `checks[]`，任一 false=FAIL | result-json 判定器（现有） |
| **P**·像素读回 | GDI 画已知图案→GetPixel/GetDIBits 读回比对，误差阈值写入 checks | 同上（程序自证） |
| **I**·注入配对 | smoke.py 经 `uitest uiInput` 注入 → 程序断言收到的消息序列/坐标写 checks | 同上（host 注入脚本+程序自证） |
| **V**·归档截图视觉判定 | 固定图案截帧 → frame.py 视觉判定器（rgba-quadrants 模式） | visual:* 判定器（现有） |
| **L**·日志模式 | wine_stderr/hilog 特征行 → 归档后 log 判定器 | log 判定器（待建，辅助） |
| **D**·宿主协议摘要 | 程序声明期望的宿主行为，宿主协议摘要落盘比对 | protocol-log 判定器（待建） |

## 3. 详细用例清单（21 域 59 例）

体系总览：**内核对象与内存（5）→ 文件/注册表/环境（5）→ 进程线程（3）→ 窗口管理（8）→ 消息调度（3）→ 输入（5）→ GDI 绘制（4）→ 屏幕（2）→ Shell/对话框/资源（4）→ 剪贴板（3）→ 网络（1）→ 异常运行时（2）→ 时钟定时（2）→ DLL/COM（2）→ 控制台（1）→ e2e 交互（4）→ 压力（2）→ 游戏与图形栈（4）**。每个域对应 Win32 API 的一个功能面，用例是该面内的可自检切片；游戏与图形栈域测 API 语义面与离屏渲染读回，渲染出图能力由现有图形烟测守（登记见 §3.20）。

规格四要素：**行为**（程序做什么）/**手段**（检测手段组合）/**通过**（无问题判据）/
**失败特征**（有问题时暴露什么——即该用例守着哪条链）。批次：P0/P1/P2/P3/P4。

### 3.1 窗口管理

**win_basic — 窗口创建与几何往返** ｜ P0 ｜ 手段 R
- 行为：注册类→CreateWindow(300,200,640,480)→ShowWindow→查询→DestroyWindow
- 通过：hwnd 有效且 IsWindow；GetWindowRect==期望（记录 DPI factor 进 metrics，物理坐标按 factor 换算比对）；GetWindowText 往返一致；销毁后 IsWindow==FALSE
- 失败特征：坐标偏差=几何/缩放链断；title 空=UTF-16 转换断

**win_zorder — Z 序操作** ｜ P1 ｜ 手段 R+D
- 行为：建 A/B/C 三窗→依次 SetWindowPos(TOP/INSERTAFTER/BOTTOM)→查询序
- 通过：GW_HWNDNEXT 序列与操作一致；D：宿主协议摘要中 z-order 变更序与之一致（摘要通道未建前此半记 SKIP）
- 失败特征：客户侧序对而宿主序错=zorder_policy/私有 Z 序链断

**win_minimize — 最小化/还原** ｜ P1 ｜ 手段 R
- 行为：ShowWindow(SW_MINIMIZE)→查询→SW_RESTORE→查询→循环 3 次
- 通过：每次 IsIconic 翻转正确；还原后 GetWindowRect 恢复原值；最小化时窗口坐标为 -32000 哨兵（上游语义）
- 失败特征：还原尺寸错=合成器猜尺寸/SC_RESTORE 握手断；还原后黑屏类由 V 附加判定

**win_maximize — 最大化与 MINMAXINFO** ｜ P1 ｜ 手段 R+B
- 行为：WM_GETMINMAXINFO 声明 min=400x300→SetWindowPos 缩到 200x150→查询实际尺寸→SW_MAXIMIZE→SW_RESTORE
- 通过：实际尺寸被 clamp 到 min；最大化铺满工作区（SystemMetrics 换算）；还原恢复
- 失败特征：min 不生效=可调整性判据/min-max 提取链断

**win_fullscreen — 模式切换与全屏** ｜ P1 ｜ 手段 R+V
- 行为：EnumDisplaySettings 记录模式表→ChangeDisplaySettings(1280x800)→建全屏窗画四象限→截帧→恢复
- 通过：EnumDisplaySettings 含模拟模式且当前模式一致；全屏窗客户区==新模式尺寸；V：四象限铺满无黑边
- 失败特征：黑边/比例错=letterbox 链；模式表缺=虚拟模式包络断

**win_owned — owner 与模态禁用** ｜ P1 ｜ 手段 R
- 行为：建 owner→建 owned(WS_POPUP+owner)→对 owner 调 EnableWindow(FALSE)→查询→恢复
- 通过：GetWindow(GW_OWNER)==owner；owner disabled 时 IsWindowEnabled(owner)==FALSE 且 owned 仍 enabled；GetWindowLong(GWL_STYLE) 含 WS_EX_MDICHILD 无关项不误报
- 失败特征：owned 判成独立顶层=受管判据/私有 owner 链断（企业微信类回归）

**win_child — 子窗口嵌套与坐标映射** ｜ P2 ｜ 手段 R+B
- 行为：父窗内建两层 WS_CHILD 各画标记→ScreenToClient/ClientToScreen 往返
- 通过：child 父子关系正确；坐标往返零误差；B：各层图案读回正确（客户端合成序）
- 失败特征：嵌套层序错=客户端合成/surface 压平链断

**win_layered — 分层窗口** ｜ P1 ｜ 手段 R+P（收敛项③验收载荷）
- 行为：SetLayeredWindowAttributes(alpha=128)→UpdateLayeredWindow per-pixel 图案→SetWindowRgn 圆角→截屏读回
- 通过：P：读回 alpha 通道==128（±2）；per-pixel 图案半透明区 alpha 正确；rgn 外区域不可见
- 失败特征：均匀 alpha 被忽略（当前已知缺口，桥实现前应 FAIL——用例先红是合法状态，收敛项落地后转绿）；rgn 不裁=shape 链断

### 3.2 消息与调度

**msg_basic — 消息循环与定时器** ｜ P0 ｜ 手段 R
- 行为：标准循环+PostMessage 自投 10 条+SetTimer(50ms) 收 10 发+PostThreadMessage(WM_QUIT)
- 通过：10 条按序收到；timer 间隔均值 40–120ms 且无丢失；循环正常退出
- 失败特征：timer 丢失=调度 starving；序错=队列语义断

**msg_order — 消息时序** ｜ P1 ｜ 手段 R
- 行为：记录创建/销毁全过程收到的消息序列
- 通过：创建序含 WM_NCCREATE→WM_NCCALCSIZE→WM_CREATE；销毁序 WM_DESTROY→WM_NCDESTROY；WM_SIZE 在 SHOWWINDOW 后
- 失败特征：时序错=win32u 消息派发回归（对 win32u 改动最敏感的哨兵）

**msg_thread — 跨线程消息** ｜ P2 ｜ 手段 R
- 行为：双线程互发 SendMessage（同步）与 PostMessage 200 条
- 通过：SendMessage 返回值正确（跨线程阻塞语义）；PostMessage 无丢失
- 失败特征：死锁/丢失=跨线程调度或 attached input 断

### 3.3 输入

**input_mouse — 鼠标注入配对** ｜ P1 ｜ 手段 I
- 行为：`--automation` 开窗并在标题输出定位标记，等待注入；断言收到的点击序列
- 通过：host 依 test.json 注入 3 次左键+1 次右键+1 次双击→程序 checks：次数、坐标（客户区换算后±2px）、按键方向全部正确
- 失败特征：坐标系统性偏移=letterbox 逆映射断；丢失=CLICK-PIPE/队列断

**input_keyboard — 键盘注入配对** ｜ P1 ｜ 手段 I
- 行为：等待焦点+注入；断言 KEYDOWN/CHAR
- 通过：注入 "Aa1中"→WM_KEYDOWN VK_A×2/VK_1、WM_CHAR 序列 'A','a','1',0x4E2D；Shift 期间 GetKeyState 正确
- 失败特征：中文断=IME/keymap 链；修饰态错=修饰键快照断

**input_relative — 相对指针** ｜ P2 ｜ 手段 I
- 行为：ShowCursor(FALSE)+ClipCursor 触发相对模式→host 注入单段 swipe（RIDEV_INPUTSINK 全局收 raw，不依赖焦点，不带 click 前缀）→注册 RAWINPUT 判定"连续步进段"之和
- 通过：连续 ≥8 条步进样本之和与注入位移一致（±20%）；GetCursorPos 被约束在 clip 矩形内；无 ABSOLUTE 串扰
- 失败特征：步进段断=REL 通道丢步变号；绝对坐标同时变化=双通道串扰
- 注：enter 定位校准（SetCursorPos）合法产生一条相对差分并进 raw 全局累计，故判定收在步进段而非总累计

**input_capture — SetCapture** ｜ P2 ｜ 手段 I
- 行为：SetCapture 后 host 注入窗外移动
- 通过：窗口外移动仍持续收 WM_MOUSEMOVE；ReleaseCapture 后停止
- 现状（2026-09-26 实测定性，保持 FAIL）：wine 服务器 capture 重定向只查 msg->win 所属线程 input 的 capture（server/queue.c find_hardware_message_window），窗外事件宿主经 root 直通链送达后 msg->win=桌面窗口（explorer 的 input，capture=0），不重定向——跨 input 的 capture 路由是 wine 上游语义限制；宿主链路无丢件（临时全量日志实锤 8 步 PTR_MOTION 全发出）。同 input 内 capture（程序自己拖动）不受影响

**input_wheel — 滚轮** ｜ P2 ｜ 手段 I
- 行为：等待注入滚轮
- 通过：WM_MOUSEWHEEL 累计 delta==注入格数×120（±舍入）
- 失败特征：delta 单位错=value120 协议断
- 注：winewayland 对 VERTICAL axis 取负（wayland 正=内容下滚，Windows 正=向前滚），smoke 编排层按 Windows 格语义取反注入，产品触控板语义不受影响

### 3.4 GDI 绘制

**gdi_primitives — 基本图元** ｜ P1 ｜ 手段 P
- 行为：离屏 DC 32bpp 画纯色线/实心矩形/椭圆/MoveTo-LineTo 折线于已知坐标
- 通过：P：各图元采样点颜色==预期（纯色，零容差）；背景未污染
- 失败特征：错色/空缺=GDI 光栅化或像素格式断

**gdi_text — 文本绘制** ｜ P1 ｜ 手段 P+R
- 行为：离屏 DC TextOut "Ag中" （设定字体）→GetTextExtent→像素抽样
- 通过：extent 宽高>0 且随字号增大；"A" 笔画采样点非背景色；中文字形非空（freetype 扫描链）
- 失败特征：中文空缺=字体扫描/locale 断；extent=0=文本度量断

**gdi_bitmap — DIB 与位块传输** ｜ P2 ｜ 手段 P
- 行为：建 32bpp DIB section 写入已知渐变→StretchBlt 到另一 DIB（1:1 与 2:1 各一次）→读回
- 通过：1:1 读回逐像素相等；2:1 中心采样颜色==源对应区域均值（±16）
- 失败特征：stride 错位=DIB 布局断；缩放采样错=StretchBlt 路径断

**gdi_palette — 调色板** ｜ P3 ｜ 手段 P
- 行为：8 位 DIB+逻辑调色板已知索引色→绘制→读回
- 通过：读回 RGB==调色板映射值
- 失败特征：错色=调色板翻译断（老游戏类依赖）

### 3.5 屏幕与显示

**screen_bitblt — 跨窗口读屏** ｜ P1 ｜ 手段 P（收敛项②验收载荷）
- 行为：窗口 A 画已知棋盘格→GetDC(NULL)+BitBlt 屏幕对应区域→读回
- 通过：读回==棋盘格（跨窗口内容可捕获）
- 失败特征：只有本进程内容/黑=捕获回灌缺失（当前已知缺口，回灌落地前应 FAIL）

**screen_enum — 显示器枚举** ｜ P2 ｜ 手段 R
- 行为：EnumDisplayMonitors+EnumDisplaySettings 全表
- 通过：≥1 monitor；主 monitor 几何与 GetSystemMetrics(SM_CXSCREEN) 一致；模式表含当前模式
- 失败特征：几何不一致=多屏枚举/虚拟屏映射断

### 3.6 剪贴板

**clip_basic — 同进程剪贴板往返** ｜ P0 ｜ 手段 R
- 行为：OpenClipboard→EmptyClipboard→SetClipboardData(CF_UNICODETEXT)→关闭→重开→GetClipboardData→比对
- 通过：往返文本逐字节相等；格式枚举含 CF_UNICODETEXT；序列号递增
- 失败特征：往返断=wine 内部剪贴板服务断（与宿主桥无关的基线）

**clip_cross — 跨进程剪贴板** ｜ P1 ｜ 手段 A+C（收敛项①验收载荷）
- 行为：实例 1 写入→退出；host 启动实例 2 读取比对（数据经宿主 pasteboard 桥）
- 通过：实例 2 读到实例 1 写入的文本/位图
- 失败特征：空/错=宿主 pasteboard 桥缺失（当前已知缺口，桥落地前记 UNSUPPORTED）

**clip_formats — 格式枚举** ｜ P2 ｜ 手段 R
- 行为：写入多格式（TEXT/UNICODETEXT/BITMAP）→EnumClipboardFormats
- 通过：三种格式全部可枚举且各格式可独立取回
- 失败特征：缺格式=格式转换层断

### 3.7 文件系统与盘符

**fs_drives — 盘符断言** ｜ P0 ｜ 手段 R
- 行为：GetLogicalDrives+GetDriveType+GetVolumeInformation 逐盘
- 通过：C: 存在且 DRIVE_FIXED；Z: 存在；Z: 根 GetVolumeInformation 成功
- 失败特征：Z: 缺失=dosdevices/盘符映射链断（ohos_file 回归哨兵）

**fs_io — 文件 IO 与路径** ｜ P0 ｜ 手段 R
- 行为：C:/Z: 各做 写→读→追加→删除→目录枚举（FindFirstFile 通配）→长路径（>260 经 \\?\）
- 通过：全部内容往返一致；目录枚举计数正确；长路径成功
- 失败特征：Z: 写失败=HOME 一致性/双实现分叉（mountmgr vs ohos_file 的回归哨兵）；枚举漏=目录语义断

**fs_watch — 目录变更通知** ｜ P3 ｜ 手段 R
- 行为：ReadDirectoryChangesW 挂起→自建/自删文件→收通知
- 通过：收到 FILE_ACTION_ADDED/REMOVED 且文件名正确
- 失败特征：无通知=watcher 后端断

### 3.8 注册表与环境

**reg_basic — 注册表往返** ｜ P0 ｜ 手段 R
- 行为：HKCU 下 CreateKeyEx→SetValue(sz/dword/binary)→Query 比对→枚举子键→Delete
- 通过：三类值往返一致；枚举计数正确；删除后查询返回 ERROR_FILE_NOT_FOUND
- 失败特征：往返断=registry 文件读写链（prefix 就绪判定的组成部分）

**env_vars — 环境变量管线** ｜ P0 ｜ 手段 R
- 行为：GetEnvironmentVariable 断言套件注入的 WINEHUA_T_TEST_KEY 等已知键；GetEnvironmentStrings 全量遍历查重
- 通过：注入键存在且值精确；遍历无重复键
- 失败特征：键缺失/错值=__env 通道/AppendStableDxvkEnv 注入链断（env 管线的可执行判据）

### 3.9 进程与线程

**proc_spawn — 子进程链** ｜ P0 ｜ 手段 R
- 行为：CreateProcess(自身 --child 模式)→等待→子进程校验父 PID 并 exit(42)→GetExitCodeProcess
- 通过：退出码==42；WaitForSingleObject 正常；子进程内 GetParent 正确
- 失败特征：spawn 失败/挂起=broker 代 spawn 链断（proc/broker 回归哨兵）

**proc_pipe — 管道** ｜ P2 ｜ 手段 R
- 行为：匿名管道父写子读 64KB+命名管道双向各 4KB
- 通过：数据逐字节一致
- 失败特征：传输错/阻塞=fd 继承/IPC 断

**thread_tls — TLS 与同步** ｜ P3 ｜ 手段 R
- 行为：TlsAlloc/4 线程各写各读+CriticalSection 并发计数+DllMain 附着记录
- 通过：TLS 互不串扰；计数最终值==操作数（无丢增）
- 失败特征：串扰/丢增=线程局部存储或锁语义断（box64/宿主线程栈回归）

### 3.10 内存

**mem_virtual — 虚拟内存与执行页** ｜ P0 ｜ 手段 R
- 行为：VirtualAlloc(PAGE_READWRITE)→写→VirtualProtect(PAGE_EXECUTE_READ)→函数指针调用该页代码→VirtualQuery 核对→释放
- 通过：调用执行成功返回正确值（小段 shellcode：mov eax,imm; ret）；VirtualQueryProtect 与设置一致
- 失败特征：执行崩=noexec 匿名 RWX 链断（ohos_virtual 回归哨兵——加壳程序兼容的根基）

**mem_heap — 堆压力** ｜ P3 ｜ 手段 R
- 行为：HeapAlloc/Free 随机尺寸 10k 次+校验图案
- 通过：无 NULL、图案零损坏、进程正常退出
- 失败特征：损坏=堆后端断

### 3.11 异常与运行时

**seh — 结构化异常** ｜ P1 ｜ 手段 R
- 行为：__try{ 除零 }__except / __try{ 写空指针 }__except / 捕获后继续执行后续断言
- 通过：两处正确进入 except（GetExceptionCode 符合预期）；之后代码正常执行；进程退出码 0
- 失败特征：崩溃=SEH 翻译链断（box64/FEX 下信号→异常路径回归）

**crt — C 运行时** ｜ P2 ｜ 手段 R
- 行为：printf/宽字符转换/locale 敏感函数/静态与动态 CRT 标志自检
- 通过：宽窄转换往返一致；输出写入 result
- 失败特征：断=msvcrt 映射/locale 断

### 3.12 时钟

**time — 时钟单调性与精度** ｜ P2 ｜ 手段 R
- 行为：GetTickCount/GetTickCount64/QueryPerformanceCounter 采样间隔检查+Sleep(100) 精度×5
- 通过：QPC 单调且频率合理；Sleep 实测 90–250ms
- 失败特征：倒退/跳变=时钟源断（frame callback 墙钟问题的用户侧观测哨兵）

### 3.13 网络

**net_tcp — 回环 TCP** ｜ P2 ｜ 手段 R
- 行为：连接宿主注入的 127.0.0.1:port（smoke 侧起 listener）→echo 4KB→关闭
- 通过：echo 逐字节一致；connect/send/recv 返回值正确
- 失败特征：connect 拒绝=socket 注入链断；传输错=ws2_32 层断（不依赖外网，可复现）

### 3.14 DLL 与 COM

**dll_load — 动态库** ｜ P1 ｜ 手段 R
- 行为：LoadLibrary(user32 之外的测试 dll，随载荷分发)→GetProcAddress 调用→FreeLibrary→重载
- 通过：函数返回预期值；引用计数行为正确
- 失败特征：加载失败=PE 加载器/依赖解析断

**com_basic — COM 基础** ｜ P3 ｜ 手段 R
- 行为：CoInitializeEx→CLSIDFromProgID("...")→CoCreateInstance 一个 builtin 类→Release→CoUninitialize
- 通过：全链 S_OK；引用计数归零
- 失败特征：断=COM 服务表/注册表协作断

### 3.15 e2e 交互（注入配对）

**e2e_click — 点击命中** ｜ P1 ｜ 手段 I+R
- 行为：开已知客户区几何的窗口画四色象限→host 注入四象限中心各一次
- 通过：四次 WM_LBUTTONDOWN 客户区坐标分别落在对应象限（±3px）
- 失败特征：系统性偏移=letterbox/fit 逆映射断；单象限错=hit-test 断

**e2e_drag — 拖动** ｜ P2 ｜ 手段 I+R
- 行为：host 注入标题栏按下-移动 200px-释放（swipe 带 button=按住拖拽）
- 通过：WM_NCLBUTTONDOWN(HTCAPTION) 到达；compositor 拖动中 wine 不收 move（xdg_toplevel.move 接管，move 计数仅作事件链路存活证明）；GetWindowRect 位移≈200px；释放后位置保持
- 现状（2026-09-26 实测定性，window-moved 保持 FAIL）：wine 的 WAYLAND_SysCommand 把 SC_MOVE 交给 xdg_toplevel_move 后位置由 compositor 接管，拖动终态没有回填通道（winehua_toplevel 只有 set_modal），wine 端 GetWindowRect 停在拖动前位置——程序按内部位置做逻辑（菜单定位/记忆位置）会错。治本方向=拖动终态位置回填（协议两端成对），落地后此断言转绿即验收

**e2e_menu — 菜单路由** ｜ P2 ｜ 手段 I+R
- 行为：程序 TrackPopupMenu 弹菜单→host 注入第 2 项
- 通过：收到 WM_COMMAND==第 2 项 ID；菜单关闭
- 失败特征：点不中/收不到=popup 路由/子窗承载断（Pad 菜单回归哨兵）

**e2e_resize — 尺寸链** ｜ P1 ｜ 手段 R
- 行为：SetWindowPos 五组尺寸→各断言 WM_SIZE/wParam、客户区 GetClientRect、宿主 configure 回执
- 通过：客户区==窗口尺寸−边框（GetSystemMetrics 换算）；wParam 标志正确
- 失败特征：尺寸错=resize configure 链断（win32u/合成器 resize 回归哨兵）

### 3.16 压力与稳定性

**stress_windows — 窗口账目** ｜ P2 ｜ 手段 R
- 行为：循环建 100 窗（交错显示/隐藏）→核对 EnumWindows 计数→分批销毁
- 通过：EnumWindows 计数一致；销毁后归零；无句柄泄漏（进程句柄数 metrics）
- 失败特征：泄漏/计数漂移=会话状态/句柄表断

**stress_messages — 消息洪泛** ｜ P3 ｜ 手段 R
- 行为：60s 内高频 PostMessage（>10k 条）+周期处理
- 通过：处理计数==投递计数；队列满时返回值正确；结束时响应正常
- 失败特征：丢失/挂死=队列丢弃路径断（InputQueue 压力同源）

### 3.17 内核同步对象与文件映射

**sync_kernel — 事件与等待** ｜ P1 ｜ 手段 R
- 行为：CreateEvent(手动/自动复位各一)→SetEvent→WaitForSingleObject；CreateSemaphore 计数；WaitForMultipleObjects 混合等待；跨进程命名事件（CreateProcess 子进程 OpenEvent）
- 通过：各等待正确返回；命名事件跨进程 signaling 生效
- 失败特征：命名事件不通=wineserver 对象命名空间断（**wineboot boot 事件挂死的历史事故即此链**——二启 explorer 全卡的根因，回归哨兵）

**filemap — 文件映射与共享内存** ｜ P1 ｜ 手段 R
- 行为：CreateFileMapping(INVALID_HANDLE)+MapViewOfFile 写已知图案→子进程 OpenFileMapping 读回比对；文件-backed 映射往返
- 通过：跨进程读回一致；文件映射与磁盘内容一致
- 失败特征：跨进程断=shm/fd 继承链；内容错=映射页缺货

**mutex_atom — 互斥体与 Atom 表** ｜ P3 ｜ 手段 R
- 行为：CreateMutex 跨进程互斥计数；GlobalAddAtom/FindAtom/GetAtomName 往返
- 通过：互斥下临界计数正确；Atom 往返一致
- 失败特征：互斥失效=wineserver 同步对象断

### 3.18 Shell、路径与公共对话框

**shell_path — 系统路径映射** ｜ P1 ｜ 手段 R
- 行为：SHGetFolderPath 全常用 CSIDL（APPDATA/LOCAL_APPDATA/DESKTOP/PROGRAM_FILES/PERSONAL…）→各路径下建删文件验证真实可写
- 通过：每个 CSIDL 返回非空路径且文件操作成功；PROFILE 指向用户目录（HOME 映射回归）
- 失败特征：路径空/不可写=shell 文件夹→OHOS 目录映射断（wineboot/profile 相关定制回归哨兵）

**shell_dialogs — 公共对话框** ｜ P2 ｜ 手段 I+R
- 行为：GetOpenFileName 弹对话框→host 注入选择文件路径并确认
- 通过：返回路径==注入选择的文件；GetSaveFileName 同理
- 失败特征：对话框不弹/选不中=comdlg32/子窗承载断

**shell_link — 快捷方式** ｜ P3 ｜ 手段 R
- 行为：IShellLink 创建 .lnk→IPersistFile 保存→重新解析
- 通过：目标路径/参数/工作目录往返一致
- 失败特征：断=shell 命名空间层

**resource — 资源加载** ｜ P2 ｜ 手段 R
- 行为：LoadIcon/LoadCursor 系统资源+LoadImage(LR_LOADFROMFILE) 位图+FindResource/LoadResource 自带资源
- 通过：句柄有效且 Draw 视为成功；文件位图尺寸读取正确
- 失败特征：断=PE 资源段解析/图形资源链

### 3.19 控制台与定时器

**console — 控制台** ｜ P2 ｜ 手段 R
- 行为：AllocConsole→GetStdHandle→WriteConsole/WriteFile→SetConsoleTitle→FreeConsole；子进程继承控制台读写
- 通过：写入成功且 ReadFile 回读一致；标题往返
- 失败特征：断=控制台后端（console 类程序的地基）

**mm_timer — 多媒体定时器** ｜ P3 ｜ 手段 R
- 行为：timeSetEvent(10ms)×100 触发+timeGetTime 单调
- 通过：回调次数 80–110；无重复句柄错误
- 失败特征：丢失/漂移=winmm 定时链（游戏循环类依赖）

### 3.20 游戏与图形栈

域定位：测**游戏程序依赖的 API 语义面**——DirectInput 输入语义、DirectDraw 2D
表面协作、D3D 设备与交换链行为。判定分三层，避免与渲染后端档位耦合：

1. **API 往返（R）**：设备创建/数据格式/协作级别/状态往返——与后端无关，
   wined3d/dxvk 档位下判据一致；
2. **离屏读回（P）**：离屏 surface Lock / GetRenderTargetData 取回已知图案——
   不依赖屏幕回灌（缺口②只挡主表面读回）；
3. **主表面/Present 像素**：受缺口②（捕获回灌缺失）限制，凡涉此先红合法；
   渲染出图能力由现有图形烟测守（`d3d8-smoke`、`d3d-switch-cube`、
   `d3d11-smoke`、`d3d12-triangle/gears/1000f`、`graphics-smoke`、
   `vulkan-smoke`——登记于此，不重复建设）。

DLL 全部动态加载（GetProcAddress，与 d3d8-smoke 同模式；mingw 不链图形
import 库）。dinput 用例依赖 C 型注入设施（--desktop-mode virtual）。

**dinput_keyboard — 键盘状态语义** ｜ P4 ｜ 手段 I+R
- 行为：DirectInput8Create→键盘设备 c_dfDIKeyboard→Acquire→host 注入键序→GetDeviceState(256B) 轮询
- 通过：DIK_A/DIK_1 状态位随注入按下/抬起翻转且无幻影键；Acquire/Unacquire 往返 S_OK
- 失败特征：全 0=设备创建/协作级别断；位错=DIK 码与 evdev 换算链断（键盘注入链的 dinput 视角哨兵）

**dinput_mouse — 鼠标轴增量与缓冲** ｜ P4 ｜ 手段 I+R
- 行为：c_dfDIMouse（相对轴）→Acquire→host 注入 move/click→GetDeviceState + GetDeviceData 缓冲
- 通过：轴通道有流且方向正确、按钮位正确、缓冲与状态通道一致；click 与 swipe 起点必须重合（enter 定位差分会吃掉/抵消轴增量）
- 现状（2026-09-26 定性）：x64 轴量级存在超注入漂移（dx 35~249 波动、dy 注入 0 实测漂至 180——wineserver 光标位移差分混入宿主合成光标管理的额外移动，FPS 视角漂移族的量化证据）；x86 轴增量确定性断流（dx 恒 0，按钮/缓冲通道正常）。两缺口记入平台缺口清单，量级以 metric 留档

**ddraw_surface — 表面协作（含调色板与 Flip 组）** ｜ P4 ｜ 手段 R+P
- 行为：DirectDrawCreateEx（动态加载）→一个程序三判定组：
  ①离屏 32bpp 表面 Lock 写已知图案→Blt→读回逐像素一致 + BltColorFill；
  ②8bpp 表面+CreatePalette/SetEntries→索引渲染读回 + GetEntries 往返
  （与 gdi_palette 对应的 ddraw 视角）；
  ③主表面+后备缓冲 Flip API 往返（像素半段受缺口②限，Flip 判定收在
  「返回值属明确 DDERR 语义集合」，独占翻转链出图由烟测守）
- 失败特征：Lock 失败=表面管理断；错色=Blt/调色板翻译断（cnc-ddraw 类
  wrap 链守卫，红警2 依赖线）

**d3d_smoke — D3D10 链守卫** ｜ P4 ｜ 手段 R
- 行为：D3D10CreateDevice（动态加载）设备创建+状态往返
- 现状（2026-09-26 定性，保持 FAIL）：dxvk legacy 档设备创建 E_FAIL——DXVK
  1.10.3 的 d3d10/d3d10_1/d3d10core 已随载荷打包（wine/dxvk/legacy/ 产物在位）
  但未启用（prefix system32 仍是 builtin d3d10，256KB vs DXVK 2.5MB 可辨），
  启用链缺 d3d10 覆盖；wined3d 档下 builtin d3d10 同样 E_FAIL。治本=DXVK
  档启用链纳入 d3d10 三件套（与 d3d11/dxgi 同路径）
- 失败特征：设备创建断=d3d10 链缺失/未启用（D3D10 程序在两档均不可用）

**d3d9_offscreen — D3D9 离屏渲染读回 + 交换链 Reset** ｜ P4 ｜ 手段 R+P
- 行为：d3d9 窗口化设备：已知色清屏+纯色三角形→GetRenderTargetData 离屏
  读回（三角内外两采样点）；窗口 resize→Reset→继续 Present（游戏 resize
  崩溃类回归哨兵）
- 现状（2026-09-26 定性，读回断言保持 FAIL）：设备创建/清屏/绘制/读回链路
  全部走通（早期 d3d9.dll 加载挂死为环境性现象，拆分独立用例后未再复现），
  但 GetRenderTargetData 读回内容与清屏色不符（整幅恒定杂色 0xff476378）
  ——wined3d 档 RT 读回内容错，游戏内截图/镜面类功能依赖
- 失败特征：读回内容错=RT 回读链断（与收敛项②同层）

## 4. 实现规范

1. **共用头**：`smoke/t/common/winehua_t_check.h`（已交付）—— `T_CHECK(name, expr, fmt, ...)` 累积 checks、`T_METRIC(key,val)`、`T_SKIP(reason)`、退出统一写 result JSON（格式对齐 result-json 判定器）。
2. **入口**：复用 `winehua_smoke_protocol.h`（`--automation/--result/--test-id/--expect`）。
3. **构建**：`smoke/tests/<id>/test.json` 声明 `build.sources/cflags/libs`（模板 `win32-driver`）；`-O2 -s`，保留 console；x64+x86 必出。
4. **C 模式注入**：test.json 新增 `inject` 字段声明注入脚本（host 侧 uitest 通道）；程序 `--automation` 进入等待+自检状态。
5. **先红合法**：收敛项（layered 均匀 alpha、capture、clip_cross、screen_bitblt）在对应实现落地前应 FAIL/UNSUPPORTED——用例先立，红转绿即收敛验收。
6. **禁令**：不引网络（net_tcp 的 listener 由 host 注入回环端口）/时间/外部文件依赖；长跑默认关；stdout 限 100 行。
7. **每批验收**：WSL 直跑 x86_64 PE 逻辑验证→设备 x64/x86→`smoke.py check` 归档绿→挂进新套件 `win32.json`。

## 5. 批次

| 批次 | 内容 | 数量 | 状态 |
|---|---|---|---|
| P0 | check.h + msg_basic、win_basic、fs_drives、fs_io、reg_basic、env_vars、proc_spawn、mem_virtual、clip_basic | 9+设施 | **已实现；2026-09-26 于 192.168.1.5/1.6 双设备 18/18 全绿**（报告 `build/automation-logs/win32-p0-verification-report.md`，归档 `win32-r20260926-034047`/`-034127`；套件 `smoke/suites/win32.json`） |
| P1 | win_zorder/minimize/maximize/fullscreen/owned/layered、msg_order、input_mouse/keyboard、gdi_primitives/text、screen_bitblt、clip_cross、seh、dll_load、e2e_click/resize、sync_kernel、filemap、shell_path | 19 | 已完成（2026-09-26） |
| P2 | win_child、msg_thread、input_relative/capture/wheel、gdi_bitmap、screen_enum、clip_formats、proc_pipe、crt、time、net_tcp、e2e_drag/menu、stress_windows、shell_dialogs、resource、console | 17 | 已完成（2026-09-26） |
| P3 | gdi_palette、fs_watch、thread_tls、mem_heap、com_basic、stress_messages、mutex_atom、shell_link、mm_timer | 9 | 已完成（2026-09-26，双架构全绿） |
| P4 | dinput_keyboard、dinput_mouse、ddraw_surface（含调色板/Flip 组）、d3d_smoke（d3d10 链守卫）、d3d9_offscreen（离屏读回+Reset） | 5 程序 | 已完成（2026-09-26，3 全绿 + 2 定性红） |

## 6. 与既有资产的边界

- 渲染出图能力（D3D8-12/Vulkan/GL 的链路通断）由现有图形烟测守：`d3d8-smoke`、
  `d3d-switch-cube`、`d3d11-smoke`、`d3d12-triangle/gears/1000f`、`graphics-smoke`、
  `vulkan-smoke`（登记见 §3.20）；本体系的游戏域（P4）测 API 语义面与离屏读回，
  两者互补不重叠
- `winehua_dinput_probe` 的 `--automation` 协议是 C 模式先例，input 域与其互补
- 收敛项映射：clip_cross/screen_bitblt=收敛项①②验收载荷；win_layered=收敛项③；win_zorder 的 D 半段=Z 序私有消息验收；win_minimize=unset_minimized 治本验收
- 三问审计缺口映射：窗口协议（3.1）、输入（3.3）、env 管线（env_vars）、进程链（proc_spawn）、noexec（mem_virtual）逐项对上
