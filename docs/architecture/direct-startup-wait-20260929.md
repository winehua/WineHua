# 冷启动等待、诊断器与桌面常亮（2026-09-29）

启动等待的诊断运行已抓到 Explorer 的锁顺序反转。第一版 `d37139a5` 虽然交替短测 12/12 通过，但随后关闭采样器的正式首轮又超时，不能称修复完成。深审发现第一版遗漏 `apply_window_pos()` 的外层递归 USER 锁；第二版 `4b92988e` 同时调整 caller/callee，完整调用链宿主回归及关闭采样器的正式 75 秒负载 **12/12 冷启动通过**，另补 Direct 运行中 resize 通过。详见 [最终验证报告](direct-startup-fix-validation-20260929.md)。诊断短测不计入性能收益样本，新旧包分别统计。

## 身份与范围

只测试当前 USB MatePad Mini。FEX、prefix、36 EXE 性能载荷保持原样；未改 Steam、默认引擎或子模块版本。修复候选只更新 Wine 的 `win32u.so`，ntdll、winewayland 和 rawfile runtime 不变。

| signed HAP SHA-256 | 本轮用途 |
| --- | --- |
| `b741c40f669798a6311dc7f4c383c061891406ccafb64b0165148044d24f9f0d` | 第一组正式 A/B；旧诊断器第三次冷启动出现退出 |
| `715fa38b1997f8d53cf54066802bf317b584d0d25a73f61af5349b2365dcc16a` | 修复信号递归、PID/映射日志与窗口常亮；第 1 轮通过，第 2 轮超时 |
| `34edf69bd6a79b904eebc3b4c4cc98f8864ea3d7d01e38f6732c50a9c07fd3ff` | 尝试进程内 `/proc/self/mem` 有界栈帧读取；4 轮通过，第 5 轮超时，无栈帧输出 |
| `8dadcfd672b61ac24431dc626bb05cd3ddd209108011310a3f91bb7da1ff931d` | 使用公开 HiDebug FP 回溯 API；第 1 轮超时，取得游戏等待桌面 WM_NULL 回复的完整栈 |
| `d9e49dba183a409b15d359beb8a2251ec99b5b9fb9bd85d987360364758a1ea7` | 增加默认关闭的 Explorer 采样参数；第 1 轮超时，取得 Explorer 更新 surface 形状时的锁等待栈 |
| `d7636555182df624672f4cbfd05977fdaf808b5fec9f14658d8b211b8e923627` | 扩展 FP 回溯到其他线程；前 6 轮通过，第二组第 1 轮通过、第 2 轮超时，抓到形成循环等待的两条线程栈 |
| `d37139a585b8e231d79257530eabe02b76620d7bfa8267b4bc688ac93c1c3e73` | 不完整的第一版：仅释放内层 USER；诊断冷启动 12/12 通过，随后正式首轮超时；桌面常亮的独立验证仍有效 |
| `4b92988e0a5a569ab26d313a605ffbc41012be369a1147efa3ed7b758f7a287f` | 第二版释放外层与内层 USER；完整函数宿主回归、正式冷启动 12/12、运行中 resize 通过 |

上述历史 signed HAP 保留在 `build/direct-loader-screenawake-20260929/`，没有覆盖唯一旧性能包。新包不能与旧样本混合统计。新增 API 仅在显式 `WINEHUA_STALL_DUMP>0` 时加载。

## 已修复的诊断器问题

原 `WineHuaStallSampleAllThreads()` 每轮执行 `sigaction(SIGPROF, handler, &previous)`。第一轮采样后没有恢复原处理函数；第二轮把自己的 handler 保存为 previous，链式调用变成自调用。PID 15541 的第二次 stall dump 后主线程及 IPC 线程发生同地址栈保护页异常，符合这个递归路径。

现在只安装一次，保留原处理函数；采样窗口外仍传递原有 profiler 信号。增加 `SA_ONSTACK`，用原子计数与发布标记保存线程记录，保留信号入口的 errno。每条等待点、PC、映射和结束记录都带 PID/TID，避免并发追加日志归属错误。

先记录 raw PC/LR/FP，再用 `/proc/self/maps` 查询地址，不调用 `dl_iterate_phdr`。LR 的 PAC 位只在查询映射时去除，原始值保留。`fileOffset` 是文件偏移，不能直接当 ELF 虚拟地址交给 addr2line。

宿主从最终生产函数提取的回归：连续 3 轮、4 个并发工作线程、原处理函数保留，以及采样窗口外链式调用均通过。Linux 宿主不覆盖 OHOS/FEX 信号链与 ARM64 回溯 API。715 和 34 包的两次真实等待均完成三次采样，未再递归崩溃。

## 已得到的真实等待点

原正式第五轮 PID 2294 的 hilog 最后只显示 `dlopen ntdll.so...`，但明确 PID 的文件记录已执行 Wine TEB/执行内存初始化，不能把缺少日志当作 dlopen 挂起证据。

715 包 PID 25825 与 34 包 PID 30074 都记录 `calling __wine_main`，随后主线程一直等待：

- `wchan=hm_futex_wait_interruptible`。
- PC 位于系统 musl。
- 去除 PAC 后的 LR 位于 `ntdll.so`，文件偏移 `0x62d04`。
- 本包 ntdll 的可执行 PT_LOAD 为 `p_offset=0x2d440 / p_vaddr=0x2e440`，因此对应 ELF 地址 `0x63d04`。
- 固定版本符号解析为 `wait_select_reply()`，`dlls/ntdll/unix/server.c:377`，即等待当前线程的 server 唤醒管道回复。

34 包未输出栈帧，所以改用已核对的公开 `OH_HiDebug_BacktraceFromFp`（API 20，文档明确异步信号安全）。对象与入口在进入 Wine 前准备，符号化不进入信号处理函数。8dad、d9 只回溯主线程，d763 扩展为所有采样线程，最多 12 层；同一对象通过原子 try-guard 防止并发调用，忙则跳过，不在信号内阻塞。

8dad 的 PID 32353 与 d9 的 PID 35163 上层栈为：

```
wait_select_reply → server_select → server_wait → NtWaitForMultipleObjects
→ wait_message → wait_message_reply → send_inter_thread_message
→ process_message → send_message_timeout → load_desktop_driver
→ load_driver → get_desktop_window
```

对应 `win32u/driver.c` 的 `send_message(hwnd, WM_NULL, 0, 0)`，用于等待图形桌面驱动就绪。游戏尚未进入正常 Vulkan 渲染循环。

d9 的主 Explorer PID 35057 连续三次采样停在：

```
musl mutex wait → window_surface_set_shape → update_surface_region
→ set_window_pos → … → process_message → send_notify_message
→ display_mode_changed
```

`update_surface_region()` 持有 win32u 的 user lock，再等待 surface mutex。当时 d9 仅采样主线程，未能确认锁持有者；下节 d763 的其他线程栈补全了锁循环。pid 35093 的另一个 Explorer 为普通消息等待，不与主桌面混淆。启动器日志的 `explorer desktop pid=35048` 实际对应 start.exe；必须用明确 PID 的 comm/栈区分其后启动的 Explorer。

ntdll、win32u 的 executable PT_LOAD 均为 `p_vaddr - p_offset = 0x1000`，本轮符号解析使用 `fileOffset + 0x1000`。这是等待点定位证据，不是 FEX、内存映射或 Direct GPU 故障证据。新增 `winehua.desktop_stall_seconds` 只为诊断启动临时向 Explorer 注入采样参数，默认 0，不持久化，正式性能 runner 不启用。

## 锁循环与修复

d763 的失败进程为主 Explorer PID 42445，游戏 PID 42525。三轮采样都能回溯其他线程：

| 线程 | 等待路径 | 已持有的锁 |
| --- | --- | --- |
| 42445（桌面主线程） | `display_mode_changed → set_window_pos → update_surface_region → window_surface_set_shape → mutex wait` | win32u USER |
| 42481（刷新线程） | `wait_objects → flush_window_surfaces → window_surface_flush → wayland_window_surface_flush → set_window_surface_contents → wayland_surface_reconfigure → wayland_surface_update_min_max → NtUserGetWindowLongW → get_win_ptr → user_lock` | surface mutex、Wayland win_data_mutex |
| 42452（Wayland 事件线程） | `waylanddrv_unix_read_events → xdg_toplevel_handle_configure → wayland_win_data_get → mutex wait` | 等待同一 Wayland 窗口数据锁 |

刷新线程的 USER 等待为 win32u 文件偏移 `0x17b784`；中间 `NtUserCallHwndParam` 为 `0x1ce57c`，OHOS helper 为 winewayland 文件偏移 `0x20ee0`（ELF 地址 `0x21ee0`），对应 `wayland_surface_ohos.c` 的 style 查询。主线程 USER → surface，与刷新线程 surface → USER 形成循环。Wayland 事件线程被刷新线程持有的窗口数据锁连带阻塞，游戏无法收到桌面 WM_NULL 回复。

第一版只修改 `update_surface_region()`，取 surface 引用和 `dwExStyle` 快照后释放其 `get_win_ptr()` 对应的 USER 锁。但 USER 是递归锁，调用者 `apply_window_pos()` 仍持有另一层，因此第一版没有断开锁循环。旧宿主测试只提取被调函数、使用非递归锁，没有覆盖这个事实；其 7 项通过和诊断 12/12 通过均不能证明修复闭环。

第二版把 `update_surface_region(surface_win)` 移到 `apply_window_pos()` 的 `release_win_ptr(win)` 之后，并保留被调函数内的 surface 引用与 ex-style 快照。入口 `user_check_not_lock()` 明确约束不能持外层 USER；所有 surface 引用均在退出时释放。DCE 失效、图标与 monitor 状态仍在原锁范围内，region 更新仍在 surface 注册、旧 surface 引用释放与驱动 `pWindowPosChanged` 之前。五个直接调用点和相关锁路径的审查见 [完整锁审查](direct-startup-lock-audit-20260929.md)。没有跳过桌面 ready 消息、增加启动 sleep 或重试。

可重复补丁为 `patches/wine/0008-win32u-surface-region-lock-order.patch`，`scripts/build_wine.sh` 已纳入补丁检查。只重建 `dlls/win32u/win32u.so` 并按 assemble 的原生库规则同步。未改变 PE、FEX 或 rawfile 载荷。

修订后的 [宿主测试](../../host_tests/wine_surface_region_lock_test.py) 原样提取完整的 `apply_window_pos()` 和 `update_surface_region()`，USER 使用递归 mutex，模拟刷新线程持 surface/Wayland 窗口锁再查询 USER。原版稳定留下 USER 深度 2、第一版稳定留下深度 1，两者都在确认等待双方后超时；第二版深度 0 完成。18 项检查包括直接/完整 caller 竞争、父窗口 surface、替换引用、RTL、查询失败、无 surface、server 失败及入口约束（预期 SIGABRT 且禁用 core）。平台/server/DCE 操作用 stub，不能当作完整 Wine 或 OHOS GPU 验证。

d371 的交替冷启动诊断共 12/12 通过（每后端 6 轮），随后正式首轮即超时，后续 11 轮未执行。其 125 个 native/rawfile 条目仅 win32u 变化，包内哈希从 d763 的 `fa080711…bcff6` 变为 `1ebe7534…6e150`。

第二版 signed HAP 为 `4b92988e0a5a569ab26d313a605ffbc41012be369a1147efa3ed7b758f7a287f`（350923640 bytes）。相对 d371，125 个 native/rawfile 条目仍只改变 `libs/arm64-v8a/win32u.so`，新包内 SHA-256 为 `b76da9d96ce1679b5453159cbc66cdaebacc23c9c7730ab518041052418a7fb2`，无新增/删除条目。该包已安装，关闭桌面/游戏采样器的 75 秒真实负载 ABBA 共 12/12 通过，无重试；随后冷启动 Direct resize 完成 960×640→800×600，878 帧、resize 后 818 帧通过，4 张截图判定通过。性能 runner 和 benchmark smoke 现禁止桌面未就绪的自动重启；失败时保全有界证据并保留 App。

## 桌面窗口常亮

原 static `ScreenAwake` 只有 EntryAbility 使用。打开桌面后 EntryAbility 正常退后台，关闭自己的常亮；DesktopAbility、VirtualDesktopAbility 和 WineWindowAbility 没有接管。锁屏导致 `aa start` 返回 10106102，与游戏加载等待是两个独立问题。

现在四种 Ability 各有独立控制实例，绑定自己的主窗口：前台 true，后台 false，WindowStage 销毁或 Ability 清理时释放。每个窗口串行应用异步请求，旧窗口完成不会覆盖新窗口，失败不会无限重试。

最终 ETS 实现的五种宿主 Promise 场景已通过：前后台请求顺序、两个 Ability 相互独立、替换窗口与释放、失败不无限重试、请求失败时消费新的生命周期状态；HAP 的 ArkTS/Native 编译与候选校验通过。

d371 实机把熄屏 override 临时设为 10000 ms，前台不操作 18 秒后 PowerManagerService 仍为 AWAKE，DesktopAbility 为 FOREGROUND，桌面窗口的 screen lock 为 state=1。Home 后 DesktopAbility 变为 BACKGROUND，常亮 API 输出 false，同一窗口 screen lock 变为 state=0。系统 override 已恢复到原 600000 ms。未等待后台自动锁屏，以免再阻碍调试。尝试通过 `aa start DesktopAbility` 直接恢复被 exported 可见性限制拒绝（10103001）；未为测试修改 Ability 导出配置，下一次正式 smoke 由 EntryAbility 启动。

## 下一步与性能边界

本次等待环的完整回溯、caller/callee 修复、fixed60 ABBA 与运行中 resize 已完成。后续性能验收继续 commands/fill 和真实游戏；禁止自动重试或只挑成功样本得出稳定性/性能结论。

旧包四个正式样本观测到 Direct 两进程 CPU 成本高约 4.8%（[历史数据](direct-performance-results-20260929.md)）；第二版完整 12 轮观测到低约 4.85%（[新包数据](direct-startup-fix-validation-20260929.md)）。跨包数据不混算，两者都不代表整体架构已达到性能目标。采样器、短诊断与常亮检查均不能证明性能收益；Zink、Audio Direct、XInput 和其余渲染验收继续属于整体计划。

本轮按版本归档的诊断、宿主回归、候选身份和最终源码见 [证据 manifest](evidence/direct-startup-wait-pad-20260929/manifest.json)。未归档签名日志、完整共享 stderr 或 HAP 本体；旧性能 evidence 目录保持独立。
