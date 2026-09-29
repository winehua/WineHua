# Explorer 启动锁循环的完整审查

## 证据与结论边界

设备 d763 包 PID 42445/42481/42452 的三轮采样给出了同一个等待环：桌面线程持 USER 等 surface，刷新线程持 surface 和 win_data_mutex 等 USER，事件线程等 win_data_mutex。游戏在 `load_desktop_driver()` 同步 WM_NULL 等桌面，尚未开始正常 Vulkan 渲染。此结论来自明确 PID/TID 的栈和源码，不来自日志最后一行。

d371 的正式首轮 `perf-amd64-fixed60-20260929-213913-1-venus` 在游戏 PID 54170 启动后 240 秒超时。该轮未开启栈采样，不能断言它必然是同一等待环。源码和修订宿主回归独立证明：d371 的锁循环确实仍然存在。

## 为何第一版失败

`sysparams.c:user_lock()` 使用递归 mutex。`apply_window_pos()` 先 `get_win_ptr(hwnd)`，原来的 `update_surface_region(surface_win)` 又 `get_win_ptr`。原版在 shape 更新时 USER 深度为 2；第一版只释放了第二次获取，深度仍为 1。只有把 region 更新移到外层 release 之后，且被调函数仍释放自己的锁，深度才为 0。

```mermaid
flowchart LR
  U[窗口线程：USER 锁] -->|等待| S[surface mutex]
  F[刷新线程：surface mutex] --> W[win_data_mutex]
  W -->|OHOS min/max 查询窗口 style| U
```

第二版删除本调用链的 USER → surface 持锁等待边；不修改同步 WM_NULL 或增加重试。OHOS `wayland_surface_update_min_max()` 的实时 style 查询仍保留，因此测试继续保留 surface → USER 回调，不能靠 stub 掉这一查询让回归通过。

不能仅把 min/max 的 `NtUserGetWindowLongW` 改成缓存就认为公共锁顺序已经正确：`wayland_win_data_get_config` 等窗口状态路径同样在 win_data_mutex 内查询 USER。即使去掉本次两线程环里的 style 查询，仍可能形成三线程的 USER → surface → win_data_mutex → USER。缓存还有 style 变化的失效同步问题。本次在 region 更新入口移除 USER → surface 边，保持 style 语义，覆盖两线程证据及这一相邻三线程拓扑。

## 调用入口逐项核对

| `apply_window_pos` 调用方 | 进入时 USER 状态 |
| --- | --- |
| `NtUserUpdateLayeredWindow` | 函数无持有 WND 引用；style/rect 与 `get_window_surface` 查询均在返回前释放各自锁 |
| `set_window_pos` | 不保留 WND 指针；位置/非客户区计算、surface 选择在调用前完成。上游 `NtUserSetWindowPos`、WM_WINE_SETWINDOWPOS、SetParent、EndDeferWindowPos 不跨调用保留 USER |
| `update_window_state` | 当前线程窗口直接更新，否则 post；无持锁 WND。SetParent、style/layered/pixel-format 修改点先释放 WND，再调用此函数 |
| `NtUserCreateWindowEx` 第一次 | create/init window 使用的两个 WND 临界区都在 hooks/消息及 surface 创建前释放 |
| `NtUserCreateWindowEx` 第二次 | NCCREATE/NCCALCSIZE 消息之后，没有重新取得并保留 WND |

入口的 `user_check_not_lock()` 与 Wine 其他驱动/消息调用入口相同，约束未来调用者；不是捕获后忽略错误，也不是用崩溃替代修复。

## 相关锁与生命周期

| 路径 | 审查结果 |
| --- | --- |
| `flush_window_surfaces` | surfaces_lock → surface mutex → 驱动 → win_data_mutex → USER；本次不改变刷新/list 管理机制 |
| `window_surface_set_shape` | 更新 shape 后会同步 `window_surface_flush`，所以单纯缩短 shape 写入临界区不能解决回调顺序 |
| `window_surface_set_clip` | surface 内部调用 GDI/driver set_clip；本次两者都在 USER 释放后执行 |
| scaled surface | 包装 surface 再进入 target surface，同样要求区域更新时不持 USER；本次没有改 DPI/缩放语义 |
| DCE 失效 | `invalidate_dce` 需要 WND/DCE 状态一致，仍在 USER 内。忙 DCE 仅置 dirty；闲 DCE 走 release/set_visible_region 到 dummy surface。检查了 DIB surface 引用替换与 Wayland surface destroy，无新增 surface mutex 获取；stub 回归不覆盖真实 GDI，仍需实机验证 |
| 窗口销毁/线程退出 | 先从 WND 摘除 surface 并释放 USER，再 register/remove 和释放 surface；此次移动与既有清理边界一致 |
| 引用所有权 | `get_window_surface` 的 caller 引用、成功 set_window_pos 为 WND 新增的引用、转交局部变量的旧 WND 引用，原来的增减顺序不变；region updater 额外获取一份临时引用，所有错误退出释放 |
| 父窗口 surface | server 的 `surface_win` 可能不同于 hwnd，仍按 `surface_win` 获取真实 surface，不错误替换为 `new_surface`。回归包含 child hwnd=43 / parent surface hwnd=42 |
| region 时间点 | server/WND 更新和 DCE 失效完成后，region 在 register/move bits/旧引用释放/驱动 WindowPosChanged 前执行；无锁期间不解引用 WND，仅使用 surface 引用和 ex-style 快照 |

GDI shape/region 操作的对象锁在返回前释放；刷新中 color bitmap 的 GDI 对象锁在进入 Wayland flush 前释放。此次审查针对实际启动环及直接相邻路径，不等于证明整个 Wine 的所有锁组合都不存在死锁。

## 可复现的对照

在 wineohos-build 内运行：

```sh
python3 /data/src/winehua/host_tests/wine_surface_region_lock_test.py \
  --baseline /data/src/winehua/docs/architecture/evidence/direct-startup-wait-pad-20260929/source-versions/winehua-window-before-surface-lock.c \
  --partial /data/src/winehua/docs/architecture/evidence/direct-startup-wait-pad-20260929/source-versions/winehua-window-partial-surface-lock.c
```

测试原样提取生产两个完整函数，使用递归 USER 锁及显式 condition variable 确保刷新线程先持 surface/win_data。原版输出 `user_depth=2`、第一版输出 `user_depth=1`，双方进入等待后两秒超时；第二版输出 `user_depth=0` 并完成。18 项检查通过，包括预期触发的入口断言。宿主 stubs 不模拟 wineserver、真实 DCE 或 GPU。

新增 replacement 用例分别检查释放 USER 后旧 surface 的引用存活，以及 caller 真正替换为新 surface 后 old/new 引用平衡、错误路径释放；空 shape/clip 的桌面常用路径也有覆盖。三线程拓扑是静态调用链推导，当前确定性动态 harness 重现的是实机捕获的两线程环，不将两者混称为已实测。

第二版 win32u 未 strip SHA-256 为 `b6e01478237e67984d0808b82efa136497c5501e3686e25f04a41a17f74aa9be`；d371 包属于第一版，不能把第二版源码/回归归入 d371 的包身份。实机以新 signed HAP 的独立 SHA、包内库比较和关闭采样器的完整负载为准。

性能 runner 同步修正了超时预算：设备测试 240 秒、host 轮询 300 秒、外层命令 360 秒。d371 失败时设备其实已写出 timeout summary，原 host 从 aa start 计时先退出数秒，导致错误报告缺少 summary。这是取证工具的边界错误，与 Wine 锁修复分别记录。

最终 `4b92988e` 完成关闭采样器的正式冷启动 12/12 和运行中 resize；全部结果、性能边界与仍需确认的范围见 [实机验证报告](direct-startup-fix-validation-20260929.md)。
