# PAL4 通过、32 位基座改为 FEX、性能定位交接（2026-09-22，截点 07:35 CST）

接续 [EXPERT_HANDOFF_20260921_2325.md](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_2325.md)。

## 0. 用户本轮给定方向（最高优先级，覆盖旧文档建议）

1. **PAL4 已能跑起来，但性能不对**：用户实测"应该 70 多 fps，慢了 3 倍多"。
2. **不要以 box64 作为 Proton 侧 32 位的运行基座**，要**调研并解决 FEX 跑 32 位程序的问题**。
   用户明确："fex 明显流畅很多，fex 的 steamapp 会崩要调查，而不是在 proton 下还要走 box64"。
3. 旧文档中"把 box64 当产品基线""steamapps 游戏必须强制 box64"的结论**已被用户否决**。

## 1. 本轮结论速览

| 项目 | 结论 | 证据 |
| --- | --- | --- |
| PAL4 白屏 | **已修复**（v33 的 X_PEND 补丁 + LAA=0）：进入主菜单 → 新游戏加载 → 进入 3D 场景，画面完全正确 | `pal4-run1-s2.png`（菜单）、`pal4-run1-now.png`（场景）、`pal4-run1-final-game.txt`（`PALIV_LOADING → PALIV_SCENE`） |
| PAL4 性能 | box64 + `BIGBLOCK=0`（唯一能跑通 box64 的档）只有 **24.8 FPS**；这正是"慢 3 倍"的来源 | `[GL-PERF] displayed=10800 fps=24.83` |
| box64 默认档 | 出厂/master 默认 `BIGBLOCK=3`（`CALLRET=2 / FORWARD=1024`）下 PAL4 **卡死在加载**：readbacks 冻结、2 核空转 | `pal4-run2-*`（readbacks 600→720/45s，utime 持续增长） |
| 32 位 FEX | **可用且明显更快**：PAL4 在 FEX 下 48 FPS 通过加载、三次进入 `PALIV_SCENE` | `pal4-fex-1.png`、`pal4-fex-game.txt` |
| steamapp | v34（FEX 基座）下 `At Home Alone Final`（`steamapps\common`，32 位 RPG Maker）**正常运行到标题画面** | `steamapp-fex-1.png`、`steamapp-fex-2.png`、`[early-fault] #10-#12 pid=28780` |
| v34 产物 | 32 位默认引擎改为 FEX，box64 保留为显式回退；已装机验证默认生效 | `HODLL=libwow64fex.dll (WINEHUA_WOW64_ENGINE=(unset))` |

## 2. 32 位后端选择：现状与本次修改

[select_wow64_backend()](F:/WineHua/proton-ohos-worktree/entry/src/main/cpp/proc/wine_child.cpp:494) 决定 `HODLL`：

| 引擎 | HODLL | 说明 |
| --- | --- | --- |
| box64 | `wowbox64.dll` | 旧默认；PAL4 需 `BOX64_DYNAREC_BIGBLOCK=0` 才能跑通，性能只有 FEX 的一半 |
| FEX | `libwow64fex.dll` | 64 位进程本来就走 `HODLL64=libarm64ecfex.dll`(FEX) |

修改（v34，已装机）：

- 32 位**默认**由 box64 改为 **FEX**；`WINEHUA_WOW64_ENGINE=box` 仍可显式回退到 box64。
- 删除"路径含 `steamapps\common` → 强制 box64"的覆盖；该判定改为只打日志
  `[WineChild] steamapps game on FEX base (exe=...)`，便于按 exe 归类崩溃样本。
- Steam 客户端进程树仍保持 FEX（原本就是）。

## 3. box64 的性能/正确性问题（作为回退路径仍需修）

- 默认档 `BIGBLOCK=3` + `CALLRET=2` + `FORWARD=1024`（`wine_env_baseline.h` Box64PerTable，master 与本树一致）
  下 PAL4 卡在加载：`winehua_gl_present_bridge readbacks` 停在 600/720，进程 utime 持续增长（约 2 核）。
- `BIGBLOCK=0` 时同一游戏可跑完，但 24.8 FPS。
- 已打补丁（v33）：`dynarec_native_pass.c` STEP0 的**跨页 guard** 与 **ninst>=inst_max** 两条提前 `break`
  给上一条指令补 `need_after |= X_PEND`。它修好了白屏，但显然没覆盖 `BIGBLOCK>=stopblock` 的**合并块**路径。
- 下一步（未做）：核对 `Extend block`/`dyn->forward` 回滚路径是否需要同样的 `X_PEND`，或该卡死属于 SMC/页保护
  失效（`unprotectDB` 保留 READ、去掉 EXEC）导致的陈旧 JIT。

## 4. FEX 侧观察到的故障特征

FEX 跑 32 位时反复出现同一形状的 host 异常：

```text
[early-fault] #11 pid=28780 tid=28780 sig=11 code=2 addr=0x140430ff0 pc=0x7ff5bdfcac lr=0x7ff5bdfb94 sp=0x1001ff2e0
[early-fault] #12 pid=28780 tid=28780 sig=11 code=2 addr=0x140430ff0 pc=0x7ff591cf08 lr=0x7ff591cca8 sp=0x1001ff2e0
```

- `sig=11 code=2` = SEGV_ACCERR（页存在但权限不允许），不是 MAPERR。
- 地址末三位固定 `0xff0`（页内最后 16 字节）——与 PAL4 原白屏 bug 的"跨页指令/跨页访问"形状相同，
  很像**栈/堆 guard page 探测**或**页尾越界访问**。
- PC/LR 都在 `<unmapped>`（即 `libarm64ecfex.dll` 的 JIT/匿名映射），不在 ELF 模块内。
- **是否良性尚未证明**：需要区分"Wine SEH 正常处理的 guard 探测"与"FEX 未认领的真实 guest AV"。
  判读方法：`[early-fault]` 是**接管前**的诊断，必须结合其后是否有 `dispatch_exception`/`code=c0000005`
  与进程是否继续存活；PAL4 与 `Game.exe` 在出这些 fault 后都继续运行了数十秒。

## 5. FEX 二进制现状（重要：设备上那颗是诊断版）

| 位置 | SHA256 | 备注 |
| --- | --- | --- |
| 设备 `wine/bin/aarch64-windows/libarm64ecfex.dll` | `a9b75e5e22a83779b2c65f2a6c7f095fe2eeb37778dc3dbc08ca77d2796e5ee3` | handoff 记为 **v21 返回缓存绕过诊断版** |
| 设备 `libwow64fex.dll` | `3c99a04135c36a2c8fa0d4a8b07afb07ca148b0604b13f864289e6f03648287b` | FEX WoW64 shim |
| 容器 `build/fex-ec/Bin/libarm64ecfex.dll`（未 strip） | `89297b45b4fabd49226d6ce968f59177bd07e3d8493f0929d42472705d425c1d` | 无 `FEX-CALLRET-CONTROL` 标记，非 v21 诊断 |
| 同上 strip 后 | `a42eb333481cad6641da5bcc2aa2f523d19bfd1799e1950a59155485628000d8` | **不等于** FEX_NATIVE_FAULT_REVIEW 记载的"原版" `8aba586e…` |

结论：设备当前 FEX ≠ 容器主构建 ≠ 文档记载原版，三者都不同。
在把 FEX 当基座之前，建议先确定"干净 FEX"的 SHA 并部署，否则性能/崩溃结论会混入诊断版本身的行为。
（v34 本轮**没有**动 FEX 二进制，保持单变量：payload 与 v33 完全一致。）

## 6. 本轮产物与设备状态

| 项目 | 值 |
| --- | --- |
| 设备 | USB `5KPBB25818203996`（旧交接用的 192.168.180.71:36875 不可达） |
| 已装 HAP | `fix32-v34-signed.hap`，SHA256 `b7c317350bb5f387b2ed5abcca82e105bc295b90692f16addfd68a6e3c061312` |
| v34 变更 | 仅 `libs/arm64-v8a/libwine_child.so`（`90a1e1306925f9a268ebcd604a52515451912d06f54ef85d5fdf53138ce0fc15`） |
| 数据分区 | 453 GB，剩 58 GB |
| 设备上运行中 | `pal4.exe`(FEX)、`mixxx.exe`(Steam, FEX) |
| 本地证据目录 | `F:\WineHua\device-evidence-20260922-fix32` |

### 关键证据文件

- PAL4/box64：`pal4-run1-s2.png`（菜单）、`pal4-run1-now.png`（场景）、`pal4-run1-final-game.txt`、
  `pal4-run1-fps-bigblock0.txt`（`267 26.417 1`）、`pal4-run1-perf1.log`（`[GL-PERF] … fps=24.83`）
- PAL4/FEX：`pal4-fex-1.png`、`pal4-fex-game.txt`（三次 `PALIV_SCENE`）、`pal4-fex-exc.txt`
- v34：`v34-install.log`、`v34-pal4-check2.log`（`HODLL=libwow64fex.dll (unset)`）
- steamapp：`steamapp-fex-1.png`（标题画面）、`steamapp-fex-check1.log`（`[early-fault] #10-#12`）
- 31 号无法落地的：本轮的 `commands.jsonl` 记录了全部 hdc 命令

## 7. 下一步（按优先级）

1. **确认 FEX 基座下的稳定性**：PAL4 与 `At Home Alone Final` 各跑一轮长时观察，判读
   `addr=…0xff0` 的 ACCERR 是否良性（对照：有无后续 `code=c0000005` / 进程是否继续出帧）。
2. **走真实 Steam 路径回归**：Steam 客户端启动游戏（v34 下 32 位游戏不再被强制 box64），
   复现并归类"steamapp 崩溃"，区分 TRGSSX/steam_api 注入、overlay、DRM 与 FEX 翻译问题。
3. **FEX 二进制对齐**：确定干净 FEX 的 SHA（文档原版 `8aba586e…` vs 容器 `a42eb333…`），
   部署非诊断版后再评估 32 位游戏性能/崩溃，避免与 v21 诊断补丁混淆。
4. **box64 回退路径**：定位 `BIGBLOCK>=2` 下 PAL4 卡死的根因（合并块/`dyn->forward` 的
   `X_PEND` 覆盖，或 SMC 页保护导致的陈旧 JIT），box64 只作为可选回退，不再作为基座。
5. Steam 完整回归与 Heaven 脚本验证（沿用 23:25 交接的验收口径）。

## 8. 复用命令（USB target）

```powershell
# 启动 PAL4（v34 起默认即 FEX；要 box64 才需要 --env WINEHUA_WOW64_ENGINE=box）
python -X utf8 tools/steam-rwx/launch_want.py --target 5KPBB25818203996 `
  --artifacts F:/WineHua/device-evidence-20260922-fix32 --label NEXT-pal4 `
  --exe 'Z:\home\PAL4\PAL4\pal4.exe' `
  --env WINEHUA_WINEDEBUG=-all,err+all --env WINEHUA_DIAG_QUIET=1

# 结束 32 位游戏
python -X utf8 tools/steam-rwx/launch_want.py --target 5KPBB25818203996 `
  --artifacts F:/WineHua/device-evidence-20260922-fix32 --label kill `
  --exe 'C:\windows\system32\cmd.exe' --arg=/c --arg='taskkill /f /im pal4.exe'
```

- 帧率现成读数：`/data/app/el2/100/base/app.hackeris.winehua/files/.wine/drive_c/windows/temp/winehua_display_fps.txt`
  （格式：`序号 fps toplevel`），配合 `hilog -T WL_EGL` 的 `[GL-PERF]` 看 take/upload/swap 分位。
- **一台设备只能同时跑一个游戏**才好比对：本轮 run4 因 run3 未退出而没有真正起来，ran2 与 ran1 的
  加载速度不可直接比较。

## 9. 不要重复的坑（本轮新增）

- 不要在 `BIGBLOCK=0` 下宣称性能结论——它只为让 box64 跑通，代价就是 3 倍慢。
- 不要用 `uitest uiInput click` 驱动 Wine 窗口：点击落在 WineHua 自己的 ArkTS 界面（本轮实测把 App 主界面
  切到前台），游戏窗口收不到。
- `At Home Alone Final` 是 RPG Maker VX Ace 项目（`Game.exe` + `Game.rgss3a` + `TRGSSX.dll` + `steam_api.dll`），
  直接启动即可出标题画面，不需要 Steam 在线也能到该阶段。
- `[early-fault]` 只在 `WINEHUA_EARLY_FAULT=1`（默认）时打印，且**先于** Wine 处理，不能单独当致命错误。
