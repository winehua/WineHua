# PAL4 白屏、Vulkan 加载器与 32 位游戏交接（2026-09-21，截点 23:25 CST）

接续 [21:10 交接](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_2110.md)。本文件记录 v25–v33 的新增证据与现场状态；旧文档中部分原因判断已被后续取证推翻，见 §1。本轮没有提交代码。

## 0. 接手先看

用户明确要求：**优先修 PAL4 白屏，PAL2 可以暂缓；保留并完善 Vulkan DLL 补丁。** 用户确认 PAL4 过去用 box64 能兼容，要求核对内存映射及 arm64 参考分支；另提示为 Steam 启动改过字体映射。这些是用户给定方向，不能把旧交接里的“先提交”“切 FEX”“保护层有问题”等建议当作新的用户指令。

| 项目 | 当前结论 | 尚未完成 |
| --- | --- | --- |
| PAL4 初始白屏 | 抓到真实 x86/JIT 字节，确认像素复制循环跨页后丢失 DEC 标志位；v33 加入补丁，实机已越过字体初始化 | 默认 BIGBLOCK=3 尚未跑完菜单/场景验证；缺严格单变量的新旧 DLL 对照 |
| PAL4 地址空间 | Proton 默认强制 LAA，与参考 Wine 不同；旧 DLL 下 BIGBLOCK=0、LAA=0 能到加载画面，LAA=1 则出现后续访问异常 | 不能据此声称所有映射问题解决；自动 LAA=0 仅写入源码，尚未打包 |
| PAL4 最新实机 | **v33 + BIGBLOCK=0 + LAA=0，23:23 已显示角色/进度条加载画面，readbacks 至少 960** | 23:25 游戏进程消失，同时宿主 PID 更换、日志重新开始；原因未定，**不能报游戏 PASS** |
| Vulkan 加载器 | v28 已完成原 Steam DLL 保持原位的新旧包 A/B，Vulkan smoke 30 帧 PASS | v28 之后完整 Steam UI/库操作回归尚未完成 |
| Heaven | v28 已显示实际 3D 飞船/城堡场景，约 3 FPS；直接启动缺必要参数是关键问题之一 | 新增便捷启动脚本本身尚未调用验证；最终包仍需回归 |
| PAL2 | GL 输入、blit 后图像完整，屏幕仍黑底白条，后续 Wayland/合成呈现仍有问题 | 用户允许暂缓；不要删除原 subsurface 祖先链修复 |

**当前设备已装 v33，不是 v24/v32。v34 不存在。PAL4 已不在运行，未留正在构建/上传的任务。**

## 1. 对旧交接的重要更正

1. **PAL4 的 `0x667fd3` 不是已证明的保护层故障。** 内存快照和反汇编显示它在游戏像素复制函数中；越界时行计数已经为负，真实 JIT 缺少本应保留的标志位计算。不能再因目录里存在 `PAL4P.drv` 就认定保护驱动缺映射。
2. **PAL2 显示链没有全部结案。** subsurface 归属已修，但后续探针显示源图与 blit 结果都完整，屏幕却异常。旧文档“剩余必为游戏自旋/保护层”的推断不成立。
3. **Steam DLL 改名不是最终修复。** 每次隐藏 Steam 校验文件会触发重新下载；v28 改在 Wine loader 上补兼容回退，并只在 Steam bootstrap 时恢复旧 `.winehua-shadow` 备份。
4. **与 arm64 参考的差异不限于默认引擎。** Proton 的 `need_override_large_address_aware()` 默认 true 是已确认的另一项差异。参考分支已有的 SIGSEGV/epilog、FS 基址修复仍要保留，但不应把故障处理器不断返回 epilog 当作游戏正常。
5. **白屏数分钟不自动等于死循环。** v33 后续快照从 `0x7d0c8c` 到 `0x7d0cb7`，寄存器也变化，代码是 PNG 解码相关循环；最后确实进入加载画面。默认配置那轮提前手动停止，未完成长时间启动验证。
6. **字体线索仍保留，但因果边界要清楚。** 当前确实选择了鸿蒙黑体；像素复制发生在字体初始化附近。已经证明的是翻译后丢失 flags，尚未证明字体替换本身造成越界，也未做完整字体回退 A/B。不要直接撤销 Steam 所需字体映射。

## 2. 工作树、设备与候选产物

### 2.1 环境

| 项目 | 值 |
| --- | --- |
| Windows 工作树 | `F:\WineHua\proton-ohos-worktree` |
| WSL | `Ubuntu-22.04`，`/home/liufeng/src/WineHua-proton-ohos` |
| Docker | 容器 `wineohos-build`，源码 `/data/src/winehua` |
| arm64 参考树 | `/home/liufeng/src/WineHua-arm64ec`，Wine 子模块叫 `thirdparty/wine` |
| 当前分支/HEAD | `feature/proton-wine-ohos` / `179507a3a8df17dbaad2c1930fe97856d9be719f` |
| box64 HEAD | `a8e7ed5ca0d5170e74d4eeb242f4bc1fa1cd449f`，另有未提交修改 |
| wine-valve HEAD | `f90646a3c5a6a54de6da03eebb24f356145c82f4`，另有大量未提交修改 |
| HDC target / bundle | `192.168.180.71:36875` / `app.hackeris.winehua` |
| 新证据目录 | `F:\WineHua\device-evidence-20260921-fix32` |
| 候选、构建脚本目录 | `F:\WineHua\proton-ohos-worktree\artifacts\fix32`（ignored） |
| 最后设备状态 | 23:25:25，`/data` 剩 58 GB；无 `pal4.exe`；宿主 PID=31250（此前=26637） |
| 最后设备日志大小 | 当前 `wine_stderr_20260921.log` 179 KB；此前为 247 MB，宿主重新启动后日志重新开始 |

Windows 树 `.git` 指向 WSL Git metadata。使用 WSL `/home/liufeng/src/...` 执行 Git；本次 `/mnt/f/WineHua/proton-ohos-worktree` 返回 I/O error。主树 status 使用 `--ignore-submodules=all`，再分别查子模块；dxvk metadata 原有问题不在本轮修复范围。

### 2.2 HAP 哈希（2026-09-21 本地重新计算）

以下文件均在 `artifacts/fix32/`：

| 文件 | SHA256 | 内容 |
| --- | --- | --- |
| `fix32-v28-signed.hap` | `7a53971d4a200671dc9e339cb554eed3566f33a73ae242821d5dedada90705f5` | Vulkan loader、Steam 原 DLL 恢复、GL 修复 |
| `fix32-v30-signed.hap` | `5825950f39b195ec67eb45bcdcca5526e9f573156c5563ac6b6aa1bad0718227` | 清除临时 PAL2 GL 探针、撤销无效 rawDPI 实验 |
| `fix32-v31-signed.hap` | `27cf8cc05a29dc0975c1670a8d9e2f8da08f8e7e5581af61683dfb00b3eddd5a` | 增加 native PE 内存快照工具 |
| `fix32-v32-signed.hap` | `67b29a255691f4af7570ea63c9c83b54c1390493897d968cf94385e23cb43a60` | GameHook 放行 NATIVEFLAGS 环境变量 |
| **`fix32-v33-signed.hap`** | **`ad9bdee9c1b4ea34836091ec8b3937a33c1239de2a251c95fd25cf3401041f90`** | 跨页 flags 补丁 + 早版 page-flags smoke，已装机 |

v33 大小 `347801526` 字节；payload SHA256：`ffb502b909561b950a9fe1528fdc1e84502c3235ac7bf79d77a7f9fb38b061fc`。

设备 `/data/local/tmp/fix32-v33.hap` 上传后已核对 SHA，再安装。23:22 再核对设备实际解压 DLL：

- `wowbox64.dll`：`adece8ddc39f4a87fee038ebcd554a39a3b7f2149ca0fd6dddc87c8ed7a94e11`，与 v33 构建一致。
- `libarm64ecfex.dll`：`a9b75e5e22a83779b2c65f2a6c7f095fe2eeb37778dc3dbc08ca77d2796e5ee3`，**仍是 v21 返回缓存绕过诊断版**。
- 沿用的 native `ntdll.so` SHA：`ea6c44bb932456a44c7d9826acc9cf1dc82295f8597293224faa7c4abd53825d`（前轮记录，本次未重建）。当前源码另有大量未打包诊断，不能盲目重编所有 ntdll 并覆盖此产物。

### 2.3 手工状态

- prefix：`/data/app/el2/100/base/app.hackeris.winehua/files/.wine`。
- 旧 `HKCU\Software\Wine\DllOverrides\vulkan-1=builtin` 手工覆盖未主动清除；旧 CEF 目录多份 `.cefshadow/.shadow2/.winehua-shadow` 备份也未做统一清理。新补丁不依赖每次隐藏原 DLL。
- `%USERPROFILE%` 下本轮 `.box64rc` 已移走，留存为 `C:\pal4-interpreter.box64rc`、`C:\pal4-nativeflags.box64rc`。不要再次把它们当作已验证的默认配置。
- `C:\process-memory.exe` 可用；`C:\page-flags.exe` 是最后一版“先执行第二页再 Flush”的 smoke，**与 v33 HAP 内的早版 smoke 不同**。
- 字体/registry 之前为 Steam 做过调整，本轮没有整体回滚。

## 3. PAL4：证据、补丁与验证边界

### 3.1 原故障是复制循环越界

故障 `guest RIP=0x667fd3 / read addr=0x2280000`。native PE 快照证明：

- `0x2280000` 为正常的 `MEM_RESERVE` 未提交区域，不能据它没有访问权限就判断 mmap 丢失。
- 源缓冲区 `0x223b8e0`；预期 `256 × 256 × 4` 复制完全落在已提交区域。
- 故障现场行偏移达到 `273 × 256`，剩余行数为 **-17**，应在 0 停止的循环仍继续。

真实 x86 指令：

```asm
00667ffd  dec ecx
00667ffe  mov [esp+0x20],edi  ; 指令本身跨页
00668002  mov [esp+0x24],ecx
00668006  jne 00667fb6
```

真实 ARM64 JIT 中 `sub w11,w11,#1` 既没有设置 ARM 标志位，也没有更新 x86 ZF，就跳到下一块 `0x668002`。关闭 `BOX64_DYNAREC_NATIVEFLAGS` 仍可复现这点。

本地证据：`pal4-code-emu.txt`、`pal4-stack.txt`、`pal4-jit.txt`、`pal4-jit.asm`、`pal4-pixel-copy.asm`。均在新证据目录，不应作为游戏二进制/内存 dump 提交到源码库。

### 3.2 box64 补丁（已打入 v33）

[dynarec_native_pass.c](F:/WineHua/proton-ohos-worktree/thirdparty/box64/src/dynarec/dynarec_native_pass.c:148) STEP 0 中两个提前结束路径：跨页 guard 与 `ninst >= inst_max`，现在都为上一条指令补：

```c
if(ninst) dyn->insts[ninst-1].x64.need_after |= X_PEND;
```

合计新增 3 行（含注释）。普通 lookahead 停块路径原本就设置了 `X_PEND`，这两个提前 `break` 漏了同样的要求。它让前一块保留下一块可能消费的 flags。

v33 实机在 LAA=0 下已完成 `palFontManagerA initialize ok`、`palScreenEffect`、`efEffectSystem`，原 `0x2280000` 重复故障未在该轮观察到。但旧默认失败样本和本轮同时有 LAA 设置差异，**严格回归仍需固定 LAA 的旧/新 DLL A/B**。

### 3.3 地址空间 A/B

Proton `virtual.c::need_override_large_address_aware()` 默认 true；参考 Wine 按 EXE 的 LAA 标志处理。PAL4 未声明 LAA。

| DLL/配置 | 已观察结果 |
| --- | --- |
| 旧 DLL，默认 BIGBLOCK=3、LAA=1 | 字体像素复制读 `0x2280000` 循环 |
| 旧 DLL，SAFEFLAGS=2 | 未解决原问题 |
| 旧 DLL，NATIVEFLAGS=0、BIGBLOCK=3 | 未解决；已核环境确实生效，并拿到缺 flags 的 JIT 字节 |
| 旧 DLL，BIGBLOCK=0、LAA=1 | 越过字体与多张 UI 图集后，读 `0x054ce037`，宿主 PC `0x7aa5d2fc` 在解释器内 |
| 旧 DLL，BIGBLOCK=0、LAA=0、NATIVEFLAGS=1 | 显示加载动画，持续出帧；后来退出；完整游戏日志记录 `PALIV_LOADING → PALIV_SCENE` 后清理 |
| v33，BIGBLOCK=3、LAA=0 | 越过字体初始化；白屏期间继续做角色 PNG 资源解码；随后被手动停止，未验证菜单/场景 |
| v33，BIGBLOCK=0、LAA=0 | 23:23 显示加载动画，readbacks ≥960；23:25 游戏已不在且宿主 PID 改变，原因未定 |

旧 DLL 的加载后退出证据：`pal4-combo-full.txt` 中 `palGameData initialize ok`、`ARENA-SAVE-CACHE: Restore [Q01 - Q01] success`、`PALIV_LOADING → PALIV_SCENE`，随后 `Terminate3D success` 与 singleton 清理。异常文件只有 `SEHException` 标题，无可用栈。不能把该轮加载画面 `pal4-combo.png` 当作游戏验证通过。

源码已新增 [apply_game_address_space_compatibility](F:/WineHua/proton-ohos-worktree/entry/src/main/cpp/proc/wine_child.cpp:195)：识别 `pal4/pal4.exe`，仅在没有显式 `WINE_LARGE_ADDRESS_AWARE` 时设为 `0`；在 entry env 覆盖之后调用，保留人工 A/B 能力。**这段仅在工作区，不在 v33。** 注释里“corrupts its UI initialization”是基于现有 A/B 的兼容性判断，尚未完成所有失效机制分析。

### 3.4 v33 最后一轮现场

启动标签 `v33-scene-trace`，PAL4 宿主 PID=29282，Wine 主线程 tid=`03f4`。关键环境：

```text
WINEHUA_WOW64_ENGINE=box
WINE_LARGE_ADDRESS_AWARE=0
BOX64_DYNAREC_BIGBLOCK=0
WINEHUA_WINEDEBUG=-all,err+all,trace+seh
WINEHUA_EARLY_FAULT=0
WINEHUA_DIAG_QUIET=1
```

未改默认 NATIVEFLAGS/SAFEFLAGS/CALLRET。启动到有内容需数分钟。诊断期间也拉起过内存工具、smoke 与 cmd 复制日志，因此这轮不是完全无干扰的稳定性测试。

- [23:23 加载画面](F:/WineHua/device-evidence-20260921-fix32/pal4-v33-handoff.png)：角色和进度条可见。
- [截点 stderr](F:/WineHua/device-evidence-20260921-fix32/pal4-v33-handoff-stderr.log)：已保存末尾 1.2 MB，含该 PID `readbacks=1…960`。
- [截点游戏日志](F:/WineHua/device-evidence-20260921-fix32/pal4-v33-handoff-game.txt)：到 `PALIV_LOGO → PALIV_LOADING` 与 `palGameData initialize ok`。
- 23:25 无 PAL4；宿主由 26637 换到 31250；设备日志从约 247 MB 重新开始，23:25:25 为 179 KB。**未观察到足以定因的 PAL4 致命异常，不把这次结束直接写成游戏自行 SEH 退出。**
- `pal4-v33-exit-game.txt` 仍停在加载阶段；`pal4-v33-exit-stderr.log` 实际已是宿主重启后的较短日志，不是完整 PAL4 退出前 trace。
- `handoff-app-status.log`、`handoff-pal4-exit-status.log`、`handoff-final-status.log` 保存时间、PID、磁盘及日志大小。

`trace+seh` 中 `RtlUnwindEx code=80000026`、线程命名异常 `406d1388`、setjmp 日志不等于致命错误。后续要结合正确 PID/线程、访问异常及应用生命周期判读。

### 3.5 page-flags smoke 的成熟度

源码：[winehua_x86_page_flags.c](F:/WineHua/proton-ohos-worktree/smoke/winehua_x86_page_flags.c)。生成跨页 DEC→JNZ，预期 256 次。

- 原 RWX 测试在旧 DLL 上 PASS，不能复现故障。
- 只设 `PAGE_READONLY` 的版本在旧 DLL 上也 PASS。
- 最新版先执行第二页 stub，再 `FlushInstructionCache`，模拟 `unprotectDB()` 清 EXEC、保留 READ 的页 metadata；**v33 上 PASS，旧 DLL 尚未跑此最新版**。
- 当前 `C:\page-flags.exe` SHA：`cb5430deab379c68843dce464916b050de5e4ef75b6353ce821ec9e75f9d305a`，上传前后已核一致；输出 `C:\windows\temp\winehua-x86-page-flags.txt`。
- v33 payload 的 `smoke/diagnostics/x86-page-flags.exe` 仍是更早的 RWX 版；下一包需要同步。
- 测试目前没有内置超时；若旧 DLL 真触发无限循环，要用外部有界观察/停止，不能无期限等待。

**目前只能说新 DLL sanity PASS，不能声称“旧 FAIL / 新 PASS 的回归测试已经完成”。**

## 4. Vulkan、GL、Heaven、PAL2 与 Steam

### 4.1 Vulkan loader 补丁已实测

[ntdll/loader.c](F:/WineHua/proton-ohos-worktree/thirdparty/wine-valve/dlls/ntdll/loader.c:3608)：应用目录的 AMD64 `vulkan-1.dll` 被 ARM64X DXVK 导入时返回 `STATUS_NOT_SUPPORTED`。此前只在 `STATUS_DLL_NOT_FOUND` 时回退 `default_load_path`，因而找不到后面的兼容系统 DLL。

新逻辑两种状态都继续搜索默认路径；若没有兼容替代，保留原架构错误。修的是 Wine 加载选择，**没有修改 Steam 原 DLL 的字节**。

确定性 A/B：设备 `C:\smoke\loader-shadow` 保留原 Steam `vulkan-1.dll`（963736 字节，SHA `8aefffb2e6ee592cf2a3b94408a1854f45952d24da4f9e89535a517385941696`）。v27 报 `c000007b/c0000135`；v28 **PASS，30 帧 present，acquire/queue 返回 0**。

证据：`vulkan-native-present-result.log`、`v28-refresh-progress.log`、`v28-loader-proof.log`。

[wine_child.cpp](F:/WineHua/proton-ohos-worktree/entry/src/main/cpp/proc/wine_child.cpp:796) 已改成只在 Steam bootstrap 时，把原文件缺失的 `.winehua-shadow` rename 回原位；不再每次 child 启动都隐藏已校验 DLL。`WINEDLLOVERRIDES` 中的 `vulkan-1=b` 保留。此部分已在 v28–v33。

### 4.2 GL 默认 framebuffer 恢复

[unix_wgl.c](F:/WineHua/proton-ohos-worktree/thirdparty/wine-valve/dlls/opengl32/unix_wgl.c:2270) `pop_default_fbo_buffers()`：单缓冲恢复用 `glDrawBuffer`，多缓冲才用 `glDrawBuffers`，避免 virgl GL 2.1 中把 `GL_BACK/GL_FRONT` 等传错 API 的 `INVALID_ENUM`。v26 起已验证，后续包保留。

PAL4 日志还有 `fs_hack_setup_gamma_shader` 使用 GLSL 3.30、当前 GL 只支持 1.20 的编译错误。它与已修的 draw-buffer 错误不同；由于同一环境能够显示加载动画，目前没有证据把它定为白屏主因，后续仍可核查其降级路径。

### 4.3 Heaven

v28 已实际进入 3D 飞船/城堡场景，证据 `heaven-scene.png`、`heaven-scenes-progress.log`。直接跑引擎需传与 launcher 相同的数据与脚本参数：

```text
-data_path ../
-engine_config ../data/heaven_4.0.cfg
-system_script heaven/unigine.cpp
-video_app direct3d11
-video_width 800 -video_height 600 -video_fullscreen 0
-sound_app null -extern_define RELEASE
```

设备游戏根目录 `Z:\home\HEAV~PVM.0\HEAV~PVM.0`。新增 [launch_heaven.py](F:/WineHua/proton-ohos-worktree/tools/steam-rwx/launch_heaven.py) 封装这些参数；**参数组合跑通过，脚本本身尚未执行验证**。旧文档把直接启动缺参数之后的表现统一归类为 32 位崩溃并不准确。

### 4.4 PAL2 暂缓

`pal2-src.png` 是完整但倒置的 intro Logo；`pal2-dst.png` 是 GL blit 后完整、方向正确的 Logo；设备屏幕仍黑底白条。下一阶段应查后续 Wayland/合成呈现。v30 移除临时 GL 探针并回退无效 rawDPI 实验，保留用户原有的 `wl_core.cpp` subsurface 祖先链修复。

### 4.5 Steam 回归未完成

v27 曾确认自检通过、CEF GPU 稳定、登录页渲染；没有完成库页面操作验证。v28 loader 补丁之后没有完整 Steam 回归，v33 又换了 box64，更不能直接标记 Steam PASS。

验收需要同时看原 `vulkan-1.dll` 保持原位、自检不反复下载、CEF GPU 不崩溃循环、界面实际响应。18:12 的旧 25 次 GPU 崩溃记录不是新失败证据；读取追加日志必须按新进程/启动时间切分。

## 5. 未提交改动与打包边界

主树 HEAD 仍为 `179507a`。原用户修改一直保留，没有自动提交或清理工作区。

| 文件/范围 | 状态与注意点 |
| --- | --- |
| `entry/src/main/cpp/compositor/wl_core.cpp` | 原 subsurface 祖先链修复，已进之前 HAP；未提交 |
| `entry/src/main/cpp/proc/wine_child.cpp` | Steam DLL 恢复已进 v28；PAL4 自动 LAA=0 **未打包** |
| `entry/src/main/ets/game/GameHook.ets` | 放行 `BOX64_DYNAREC`、`BOX64_DYNAREC_NATIVEFLAGS`、`WINE_LARGE_ADDRESS_AWARE`；v32/v33 已含 |
| `thirdparty/box64/wine/wow64/wowbox64.c` | 保留之前按参考恢复的 JIT 非 SMC 故障 → epilog 逻辑；未提交 |
| `thirdparty/box64/src/dynarec/dynarec_native_pass.c` | 本轮跨页/指令上限停块补 X_PEND，v33 已含；未提交 |
| `thirdparty/wine-valve/dlls/ntdll/loader.c` | Vulkan 架构不匹配回退补丁，v28 起已含；未提交 |
| `thirdparty/wine-valve/dlls/opengl32/unix_wgl.c` | 单/多 draw buffer 恢复修复，已打包；未提交 |
| `scripts/package_payload_entry.py` | 原有新增 payload 单文件/整 payload 替换器；未提交 |
| `tools/steam-rwx/process_memory.c` | native PE 只读内存快照工具，v31 payload 已有；当前源码另清除了一个初始化 warning |
| `smoke/winehua_x86_page_flags.c` | 新增测试，当前比 v33 内版本新；旧 DLL FAIL 对照未完成 |
| `tools/steam-rwx/launch_heaven.py` | 新增启动脚本，待调用验证 |
| `tools/steam-rwx/prepare_native_probe.py`、`window_snapshot.c` | 诊断工具，未提交 |
| `tools/steam-boundary/window-response.c`、`window-response2.c` | 原工作区未跟踪工具，保留；未归入本轮核心修复 |

Wine 子模块还有 `thread.c`、`unix/env.c`、`unix/loader.c`、`ohos_virtual.[ch]`、`server.c`、`signal_arm64.c`、`sync.c`、`unix/thread.c`、`unix_private.h`、`virtual.c`、`win32u/font.c`、platform-process-smoke 等脏修改（总计 15 个文件）。**不要认为这些源码都已进入当前 HAP，更不要把它们一并重建当作单变量实验。**

`thirdparty/box64/AGENTS.md` 含上游 AI 贡献限制；当前为用户明确授权的本地兼容修复，未准备上游 PR。未来对外贡献须另行核对贡献规则与披露要求。

## 6. 可直接复用的操作方法

### 6.1 构建、安装

已有脚本 `artifacts/fix32/build_v33.sh` 顺序完成：增量编译 wowbox64、strip、编 smoke、从 v32 定点替换 payload、签名、验签。`package_v33.py` 检查 HAP 变化范围。

```powershell
wsl -d Ubuntu-22.04 -- docker exec wineohos-build bash /data/src/winehua/artifacts/fix32/build_v33.sh
```

该脚本再运行会覆盖已有 v33 产物；继续开发应新建版本号并保留现有基线。仅编 box64 的容器内命令：

```bash
make -C build/box64-pe/wowbox64-prefix/src/wowbox64-build -j8
```

脚本 `build_v28.sh` 中有 `wine_child` 单目标 CMake 编译、strip 路径，可用于下一包。重打下一版时，只替换已确认需要的库和 payload 项，不引入其他脏源码产物。签名日志 `vNN-sign-private.log` 可能含口令，不打印、不外发。

HDC wrapper 会留本地命令账本，示例：

```powershell
python -X utf8 tools/steam-rwx/device_command.py --target 192.168.180.71:36875 --artifacts F:/WineHua/device-evidence-20260921-fix32 --label UNIQUE --show -- shell 'COMMAND'
```

HAP 约 348 MB，上传约 60 秒，wrapper 用 `--timeout 120`。`hdc file send/recv` 的 Windows 本地路径用反斜杠；等上传进程完成、核 SHA 后，再执行：

```text
aa force-stop app.hackeris.winehua
bm install -r -p /data/local/tmp/fix32-v33.hap
```

### 6.2 PAL4 重现

从 Windows 工作树根目录执行，标签换成新的，避免覆盖之前证据：

```powershell
python -X utf8 tools/steam-rwx/launch_want.py --target 192.168.180.71:36875 --artifacts F:/WineHua/device-evidence-20260921-fix32 --label NEXT-pal4 --exe 'Z:\home\PAL4\PAL4\pal4.exe' --env WINEHUA_WOW64_ENGINE=box --env WINE_LARGE_ADDRESS_AWARE=0 --env BOX64_DYNAREC_BIGBLOCK=0 --env WINEHUA_WINEDEBUG=-all,err+all --env WINEHUA_EARLY_FAULT=0 --env WINEHUA_DIAG_QUIET=1
```

验证默认 BIGBLOCK 时删去其 env 项，仍显式固定 LAA=0。未打包的 PAL4 自动 LAA 代码不能替代这里的显式参数。

停止 PAL4：通过同一 launch 工具启动 `C:\windows\system32\cmd.exe`，参数 `--arg=/c --arg='taskkill /f /im pal4.exe'`。`aa start` 成功仅代表启动请求成功；看 `hilog -T GameTest -z 6`、进程、游戏日志与截图确认实际启动。升级解包可能额外耗时约 30 秒。

### 6.3 只读内存快照

[process_memory.c](F:/WineHua/proton-ohos-worktree/tools/steam-rwx/process_memory.c) 枚举目标进程、`VirtualQueryEx` 低 4 GB 非空映射，对每个参数地址读取 256 字节。启动设备已有的：

```text
C:\process-memory.exe pal4.exe 0x667fc0 0x1004059c0 0x329f18
```

输出 `C:\windows\temp\winehua-process-memory.txt`，约 623 KB，需等写完再收回。地址为示例，下一轮不可照抄 emu/ESP：`[SMC] x0_in` 为 emu；前 16 个 uint64 是寄存器（eax/ecx/edx/ebx/esp/ebp/esi/edi…），offset `0x80` 为 flags，`0x88` 为 RIP。

应用内复制 payload 文件可用：

```text
copy /b /y \\?\unix\data\storage\el2\base\files\wine\smoke\diagnostics\process-memory.exe C:\process-memory.exe
```

**必须 `/b`**：不加时 cmd copy 遇 `0x1a` 会截断 EXE。hdc shell 能看到的 `/data/local/tmp` 对 app 不可见，不能从这里直接 cmd copy；app 内 `Z:\` 映射也不等于 hdc shell 的文件视图。

小 PE 可用 ignored 的 `artifacts/fix32/stage_hex_probe.py` 分块经 Want/cmd 写连续 hex，再 `certutil -decodehex`。当前脚本硬编码 `x86-page-flags-oldtest.exe → C:\page-flags.exe` 和标签，复用前要改名。hex 不能换行或带尾空格，传完核 SHA。`build_probe.sh` 可编当前测试；Capstone 已装在 ignored `artifacts/fix32/python-tools`。

### 6.4 避免重复踩坑

- `.box64rc` 测试曾卡在 `7B228F18` 初始化临界区，未进入游戏，不能当成有效游戏 A/B。
- `cmd set BOX64_*` 不等于进入 child native `__env`；有效覆盖走 GameHook Want 白名单，读取真实启动环境/`BOX64ENV` 核实。
- `nativeflags-env-proof.log` 那轮缺 NATIVEFLAGS，是无效对照；`nativeflags0-late.log` 明确 NATIVEFLAGS=0，是有效失败样本。
- `pidof pal4.exe` 在本轮多次没有结果，但 `ps -A | grep -i pal4` 有进程；不要仅凭 pidof 判退出。
- 宿主 `exit=11` 不等于 smoke 失败，必须看 smoke 自己的结果、帧和退出阶段。
- `uitest keyEvent` 未可靠到达 Wine，不作为键盘响应验收。
- 用 `hilog -T GameTest -z 6`，不是 `-x -z`；设备锁屏会让 `aa start` 失败。
- `WINEHUA_DIAG_QUIET=1` 能关闭反复 SMC 日志；`trace+seh` 仍会打印大量正常 unwind/setjmp。日志按 PID 与时间取有界片段，及时落本地；宿主重启会使当前日志丢失前一轮内容。
- 不要看到旧 PID 的 readbacks 就认为新游戏已出帧。本轮追加日志里同时有旧 PID=20489 和新 PID=29282。
- 游戏、smoke、cmd 共享宿主/显示环境；正式稳定性验证尽量只从 hdc 观察，减少中途启动其他 Wine 程序。

## 7. 下一位的执行顺序与完成标准

1. **先读本文件和 v33 截点证据，不重新从保护层假设开始。** 确认设备 DLL SHA、无遗留游戏进程；保留原修改与现有候选。不要自动提交所有脏文件。
2. **完成 PAL4 后半段定位。** 固定 v33、LAA=0，记录启动时间、宿主 PID、PAL4 PID；先做无其他 Wine 进程打扰的一轮。观察真正菜单/场景，若退出，及时保存游戏日志、Wine 异常及宿主生命周期证据。区分游戏退出、宿主重启与纯加载耗时；此次 v33 只证实到加载画面。
3. **补严格 flags 回归。** 固定 LAA、NATIVEFLAGS、BIGBLOCK，对最新版 SMC page-flags smoke 做旧/新 DLL A/B。若旧仍 PASS，则该测试还不是复现器；继续以真实 JIT 为依据改进，不虚报回归覆盖。默认 BIGBLOCK=3 也需完整游戏验证，当前没有证据要求把所有游戏永久改成 BIGBLOCK=0。
4. **验证并打包 PAL4 自动兼容设置。** 若 LAA=0 确认必要，下一包重建 `libwine_child.so`，带上该设置及既有 Vulkan 恢复逻辑，并同步最终 smoke。用普通游戏启动验证自动生效，显式 env 仍可覆盖。不要把 v34 写成已经构建。
5. **补 Steam 完整回归与 Heaven 脚本验证。** Steam 原 DLL 不隐藏，CEF 稳定且界面实际响应；Heaven 真正出 3D 场景。PAL2 按用户要求暂缓。
6. **收敛补丁与记录。** 分清原用户修改、产品修复、诊断工具；清理仅本轮确认无用的诊断前先保留证据，不提交私有签名日志、游戏 PE/dump 或账号数据。

PAL4 完成标准是进入可操作的菜单/场景并稳定运行，不能用字体初始化成功、加载动画、smoke PASS 或宿主退出码代替。Vulkan 的加载器问题已有独立 A/B 证据，Steam 全链路仍需单独验收。
