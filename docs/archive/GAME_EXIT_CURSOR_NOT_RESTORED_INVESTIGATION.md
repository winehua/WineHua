# 游戏退出后主窗口光标不恢复 —— 调查记录

> 状态：已修复（2026-09-22 归档）。文档中分析出的两处缺陷已分别修复并合入 master（b99a0c2、fa13f36）。本文仅作历史记录。

日期：2026-09-09
状态：**代码分析完成，未验证**（待设备日志判别）

## 现象

某些游戏（真相对模式，如 dinput 视角游戏）进入游戏时隐藏系统光标，正常;游戏退出后回到主窗口，**光标保持消失，不会恢复**。

## 光标显隐链路（现状代码）

```
wine 侧游戏藏光标+挂约束 (needs_relative 判定)
  → wine 创建 zwp_relative_pointer_v1 对象     (wayland_pointer.c:1025-1033)
  → C++ PointerExtras::relmgr_get_relative_pointer   pointer_extras.cpp:256
      → ApplyHostCursorLock(true, tl)  冻结系统光标 + 回调 cb(true)
  → NAPI tsfn (napi_init.cpp:893) → ets WineWindowManager.ets:211
      → pointerLocked=true → applyPointerVisibility() → setPointerVisible(false)

恢复唯一入口:
游戏窗口退出 → wine 销毁 relative_pointer 对象
  → C++ OnRelativePointerDestroyed   pointer_extras.cpp:283（剩余 0 个时）
      → ApplyHostCursorLock(false, 0) → cb(false)
  → ets pointerLocked=false → applyPointerVisibility() → setPointerVisible(true)
```

## 发现的代码级缺陷（两个都可能导致不恢复）

### 缺陷 1：C++ 锁/解锁回调不对称（pointer_extras.cpp:350-431，实锤）

- 锁方向：`LockCursor` 即使对全部注册窗口失败（`locked=0`），`cb(true)` **仍无条件发出**;
- 解锁方向：`ApplyHostCursorLock(false, 0)` 只在 `lockedWindowId_ != 0` 时才发 `cb(false)`，
  未锁过 → 直接 return → **cb(false) 永不发出** → ets `pointerLocked` 卡死 true。

触发条件（LockCursor 失败）：OH_WindowManager_LockCursor **仅支持获焦窗口**（oh_window.h 文档），
且部分设备/系统返回 DEVICE_NOT_SUPPORTED。exclusive 全屏游戏（fullscreenPopup 子窗口
`setWindowFocusable(false)`，PopupWindowManager.ets:162）不可能获焦 → 该场景必现缺陷 1。

### 缺陷 2：多客户端"全归零才解锁"（pointer_extras.cpp:287-298，设计缺陷）

`relativePointers_` 跨全部 wl_client 收集。wine 每进程一条 wayland 连接：
桌面 root（explorer 进程）启动瞬时也建过 relative_pointer（"桌面误入相对模式首击失效"
d63342f 已实锤）。游戏退出时游戏客户端对象销毁后 `remaining` 仍 ≥1（root 的还在）
→ 不解锁、不通知 → ets 卡死。

root 侧对象何时清掉：要等 explorer 自己再走一次 ClipCursor/SetCursorPos
(wayland_pointer.c:1154 update_constraint 调用点) 才销毁，可能长期存活。

## 判别方法（现有打点，无需改代码）

复现后抓 `WL_PtrExt` + `[MW]PtrVis`：

```bash
H="hdc -t <dev>"
$H shell "hilog -z 5000 -t app" | grep -E 'PtrExt|PtrVis'
```

- 锁定瞬间全部 `LockCursor win=... failed ret=...` → 缺陷 1
- 退出后 `relative_pointer destroyed (remaining=1)` → 缺陷 2
- `host cursor LOCKED` + `destroyed (remaining=0)` 但无 `[MW]PtrVis → SHOW` → NAPI/tsfn 层

## 修复方向（未实施）

1. 缺陷 1：`cb(false)` 与 IPC 解耦——检测到游戏相对模式结束就**无条件**通知 ets，
   `UnlockCursor` IPC 仅在锁过时做。
2. 缺陷 2：relativePointers_ 条目记录来源 toplevelId（relmgr_get 时已算好）；
   解锁判定改为"剩余对象中没有非桌面 root 的"——root 自身对象不参与解锁判定。

## 验证记录

- 2026-09-09 23:46 在 arm64 真机上抓取一次（本轮运行 graphics_smoke，
  非光标复现——smoke 不藏光标，链路未触发，日志仅 1 行窗口注册）。**待用真相对模式
  游戏（PAL2/红警2/模拟邻居）重跑。**
