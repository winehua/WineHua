# Steam 64 位平板调试交接：字体已修复，输入停滞与多窗口显示待定位

更新时间：2026-09-20 15:50，中国标准时间。交接对象：接手本地工作区与平板的 AI。

## 0. 先读这段，不要从头试参数

用户目标是让 Windows Steam 在鸿蒙平板上正常显示、操作、登录进入库页并重启可用。路线仍是 **Windows Steam + 原生 ARM64 Wine + AMD64 ARM64EC/FEX**；I386 使用 wowbox64 作为对照。

本轮已经完成一项有前后对照的字体修复，并部署到平板：DirectWrite 原先只有 1 个字体家族，修复后 226 个；I386/AMD64 字体回归均 PASS。Steam 已能越过 AfterCreated，出现 BrowserReady/SetName/renderer，显示中文登录 HTML 和主窗口框架。**不能继续笼统声称“64 位始终启动不了”，也不能再把当前卡顿当成已修复的字体递归。**

当前未解决：Steam 主网页区域黑屏、主窗口点击不响应、好友列表显示/更新异常。最新证据确认 Steam 自行记录 webhelper IPC 超时并重启；重启后的窗口 WM_NULL 消息也超时。Wayland 点击发送与主窗口身份对上，Wine 开始菜单可响应。多 native 窗口合成另有需要验证的结构性限制，但未证明它解释所有症状。

用户反感反复试 GPU/栈参数、频繁重启、没有对照的补丁堆积。要求充分分析、单点修复、核验实机，不要用“又启动了一遍”代替进展。此次用户要求整理交接，**交接时没有再重启/停止 App、没有部署新的输入/图形行为补丁**。

## 1. 环境、入口与当前状态

| 项目 | 值 |
| --- | --- |
| Windows 工作区 | `F:\WineHua`，PowerShell |
| 当前开发树 | `F:\WineHua\proton-ohos-worktree` |
| WSL 对应目录 | `/home/liufeng/src/WineHua-proton-ohos`，Ubuntu-22.04 |
| 分支 / HEAD | `feature/proton-wine-ohos` / `32efc9c709d19a973942f967e96cb062bd84f2f7` |
| Docker 构建容器 | `wineohos-build`，源码挂载 `/data/src/winehua` |
| OHOS 工具 | 容器 `/apps/harmony`，SDK `/apps/harmony/sdk/default/openharmony` |
| LLVM MinGW | `/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64` |
| 平板 hdc target | `5KPBB25818203996` |
| Bundle / Ability | `app.hackeris.winehua` / `EntryAbility` |
| App 文件根 | `/data/app/el2/100/base/app.hackeris.winehua/files` |
| 当前 prefix | 上述根下 `.wine`，container_id=`default` |
| Steam 路径 | `C:\Program Files (x86)\Steam\steam.exe` |
| Steam 日志 | prefix 下 `drive_c/Program Files (x86)/Steam/logs` |
| Wine stderr | `/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_20260920.log`，大文件，只取有界尾部 |
| 私有证据目录 | `F:\WineHua\device-evidence-20260920-steam-review` |

15:49:47 交接检查 App 宿主 PID 30473 仍在运行。Steam 已自行更换过 webhelper 世代，最后日志 15:49:41 仍有 BrowserReady。**这些 PID/时间是快照，不是下一轮可直接使用的固定值。** `ps STIME` 与应用日志时钟看起来不一致，判断同轮优先使用设备 `date`、日志日期、进程身份和创建/退出事件，不能只看 ps 的 STIME。

账号由用户输入；本轮已有登录态自动推进，不索取密码、不清账户数据。早先用户曾允许重装且已自行完成，不能把这当成后续任意清 prefix 的理由。

## 2. 已确定的原因与已经完成的修复

### 2.1 较早的导入问题：已修复，不是当前卡点

Proton DLL 重定向曾错误处理 `steamclient64.dll` 导入，已有明确修复。不要重新用它解释现在的无响应。

### 2.2 字体初始化递归：有明确机制及对照

保存的探针现场：宿主 browser PID 9174 / Windows PID 696 / TID 700。CEF 创建请求字体和 sans fallback 都失败，进入 `PlatformFontSkia::InitFromDetails` 的 `!success` 分支，调用 `EnsuresDefaultFontIsInitialized`，重入尚未完成的 `SystemFonts::Initialize/AddFont`。20 次 AddFont，相邻 RSP 全部下降 `0x700`/1792 字节。

这不是已证明的 FEX 回调返回栈破坏：记录中 DirectWrite COM 回调正常返回。该探针本身改变时序，且用过 32 MiB 栈；不能把这次最终 `c0000354`/Quit 与原 DLL 的所有 SIGSEGV 等同。

原 prefix 字体状态：HKCU External Fonts 240 项，HKLM NT Fonts 22 项，其中 20 项相对路径指向空的 Windows Fonts；真实 TTF 在运行时和系统目录。DirectWrite 从 HKLM 表构建集合。Wine `update_external_font_keys()` 看到 HKCU 已匹配，就跳过同步系统表，不会自愈缺项。

此前字体 family 替换和 936 **已经产品化**在 `wine_child.cpp::ensure_prefix_fonts_and_codepage()`；遗漏的是 DirectWrite 系统登记的一致性。不要说“以前完全没进代码”，也不要把差异解释成完全独立的 32/64 字体表：Wow6432Node Fonts 是主表链接，本次初始化层共用。

本次修复位置：`thirdparty/wine-valve/dlls/win32u/font.c`。

- 新增 `sync_external_font_key()`：补缺项；不同路径仅在旧路径不存在时补正；有效用户字体路径保留。
- HKCU 已匹配分支也同步 HKLM NT/Win9x Fonts。
- 版本化补丁 `patches/wine/0001-win32u-repair-external-font-registration.patch`；`scripts/build_wine.sh` 幂等应用。
- 已编译 win32u.so，已同步 `entry/libs/arm64-v8a/win32u.so`，已用隔离候选 HAP 安装验证。

### 2.3 实测结果与限制

| 测试 | 修复前 | 修复后 |
| --- | --- | --- |
| AMD64 字体家族 | 1 | 226 |
| Tahoma / 鸿蒙拉丁 / 鸿蒙中文 / sans | 全 FAIL | 全 PASS |
| AMD64 回调次数 | 12，失败即停 | 512，正常返回 |
| I386 同源测试 | 本轮未取修复前基线 | 226 家族、四项 PASS、512 回调 |
| 最终 HAP 内 steam-font suite | — | 整体 PASS，I386 宿主 PID 32436、AMD64 32446 |

真实 PE Machine 为 I386/AMD64。老的 `smoke/x64` 很多产物实际是 ARM64 PE `0xaa64`，不能拿它充当 AMD64→FEX 验证。AMD64 实际使用原 FEX，I386 日志显示 wowbox64。

修复会话后 HKLM NT Fonts 落盘 254 项，原有效 HarmonyOS Sans Digit 路径保留。但磁盘快照中仍有 Tahoma 相对路径：**不要宣称所有坏路径均被清掉**。DirectWrite 实际查找、MapCharacters/CreateFontFace 均已通过；任意自定义字体路径保护未专门注入测试。字体表最初被谁破坏仍未知。

## 3. 最新无响应/窗口问题：事实、推论与禁区

### 3.1 两个浏览器世代，不能混为一个现场

**字体修复后的首轮（15:27—15:32）：** browser Windows PID 608，15:27:46 AfterCreated，15:27:47 SetName/BrowserReady/renderer；登录 HTML 中文背景可见，随后自动登录。主窗口后续黑屏。15:32:29 主窗口 `(host 28031, surface 44)` / toplevel 8 / producer `0x6da000000022`，帧计数 15838，提交和绘制年龄约 5 ms。帧计数不证明像素变化或 UI 健康。

**最终字体包运行会话（15:38 起，最新输入调查）：**

- 原 browser Windows PID 1044 / 宿主 33090；GPU 宿主 33125。
- 主窗口 `(33090,32)` / toplevel 6 / producer `0x816500000019`，1280×780；好友窗口 `(33090,43)` / toplevel 8 / producer `0x816500000022`，300×650，位置 `(500,85)`。
- 15:42:49 点击物理 `(280,94)` 的“库”，换算 Wine `(140,47)`，命中 tl=6；enter、button down/up 均发给匹配客户端，`sent=1`。这只证明发送，**未证明 Wine 接收、Windows 取消息、CEF 消费**。
- 15:44:03 好友 producer 帧龄 223422 ms。日志 `ProducerDead` 只是超时分类，不等于进程已经死亡。好友列表是窗口，不能仅凭标题称它为独立进程。
- Wine 开始按钮和菜单可以响应、显示，证明不是整个输入链/SHM 合成失效。
- **15:44:44：Steam 日志记录 `Failure pushing command to webhelper 0/16704`、`PushCommand: Restarting webhelper process due to IPC timeout`，关闭 Windows PID 1044，启动 PID 2988。** 这是输入无响应之外的独立停滞证据。IPC 超时不等于已证明同步原语有 bug。
- 15:44:56 原 browser/子进程退出，tl=6/8 销毁；退出期间 GPU/renderer SIGSEGV 不可直接当作超时首因，必须看时序。

**自动重启后：** 原生 ARM64 小探针两次对 PID 2988 / TID 2992 / HWND `0x2014e` / class `SDL_app` / title Steam 发 WM_NULL，返回 ERROR_TIMEOUT=1460。首次 501 ms，第二次 SMTO_ABORTIFHUNG 立即拒绝。同会话 shell/菜单返回正常。采样是新世代，不能说已取得旧 PID 1044 的栈。WM_NULL 超时也不直接指出是哪一个调用阻塞。

### 3.2 合成层的另一个可疑点

`entry/src/main/cpp/graphics/egl_renderer.cpp`：每实例只有一个 `zeroCopySurfaceKey_`，已注册时保持该源。绘制 CPU 桌面后，再画一个 external-OES native 层，并回填遮挡区。

`entry/src/main/cpp/compositor/frame/zc_bridge.cpp::GetZeroCopyOccluders()`：有全零 SHM 占位绕过逻辑；当前源码按 FrameSerial 缓存，别再描述成每帧全扫。

这套模型的多 native 窗口保帧/层序需要单独验收，可能解释好友列表显示异常。但**目前没有同帧像素证据证明它是本次黑屏的唯一原因**。输入层按窗口层序选择目标，如果显示层只画一个源，理论上可能出现“看到 A、点击 B”；本次点击库的点位却与主窗口对上，应准确限定结论。

## 4. 下一步推荐调查顺序

### 第一优先：同世代的窗口消息消费闭环

1. 先拍当前页面、取设备时间/进程列表和 webhelper 日志尾部，标记 browser Windows/host PID、UI TID、HWND、Wayland surface、producer。若自动更新或重启，另开轮次。
2. 无破坏消息响应探针复测；不要先把 UI 卡住归为坐标错。
3. 对固定点击关联：鸿蒙事件 → compositor 目标和坐标 → Wine Wayland 回调 → Windows 窗口线程取消息 → CEF UI 处理。找第一个缺失边界，再加局部、默认关闭的诊断。
4. 在无响应窗口 UI 线程上取得可解释的等待/调用位置；既记录 Windows TID 也记录宿主 TID。只看到 NtWait 入口不算死锁，必须看返回、拥有者、任务来源。
5. 只有取得“通知已发但同世代对象未消费”的证据，才考虑 Wine/FEX 同步契约。不要直接根据 Steam 的 IPC timeout 改事件语义。

### 第二优先：多 native 窗口合成独立最小复现

- 两个不同尺寸、不同颜色、可点击更新计数的 native 窗口；其中一个停止重绘，另一个持续更新。
- 验证静态窗口保留最后一帧、切换前后关系、遮挡、拖动、点击与所见一致。
- 同时记录逻辑窗口身份与 producer 所属，不只靠尺寸；区分同尺寸接替、旧世代残留、有效静态窗口。
- 若证实单 producer 绘制限制，修复应是每层保留纹理并按共同 z-order 合成；不能靠“选最新 producer”或排除好友窗口凑结果。
- 先取源 buffer 与最终合成同帧像素：源已黑查 renderer/GPU；源正常但输出黑查合成。计数增长不替代像素证据。

### 验收与止损标准

每轮只改有证据的一点，固定运行时/客户端/profile。启动验收需要同世代 BrowserReady、SetName、实际 renderer；功能验收需要实际页面、点击/键盘/页切换、库页和好友窗口、退出重开。最后关闭新增诊断，三次冷启动。**目前仅字体回归和启动握手过关，整体功能/三次冷启动未过。**

## 5. 代码、构建产物与身份

本轮新增/修改重点：

- Wine：`thirdparty/wine-valve/dlls/win32u/font.c`，独立 Wine 工作树，diff 为本次字体补丁。
- 主树：`scripts/build_wine.sh`、`scripts/assemble.sh`、`scripts/check_runtime_components.py`、`patches/wine/0001-win32u-repair-external-font-registration.patch`。
- 字体测试：`tools/steam-boundary/font-contract.cpp`、`scripts/build_steam_font_contract.sh`，产物 `artifacts/steam-font-contract/font-contract-{amd64,i386}.exe`。
- `steam-font` suite 已接入；构建检查 suite、PE Machine、文件 SHA256。默认测试还写 `C:\font-contract-{amd64,i386}.jsonl`；`--output` 写 smoke summary JSON。JSON backend 占位不算后端证据，另查运行日志。
- 响应探针：`tools/steam-boundary/window-response.c`；编译产物 `artifacts/steam-font-contract/window-response-arm64.exe`；输出 `C:\window-response.txt`。仅可见窗口，最多 40，WM_NULL 500 ms，有上限；不是 debugger/线程栈工具。
- 隔离打包：`scripts/package_font_candidate.py`，依赖 `artifacts/steam-font-contract/baseline.hap`，保留基线 App/其他运行库，只换 win32u、测试和清单，逐条比较 HAP 内容。

| 已部署最终字体候选身份 | SHA256 |
| --- | --- |
| `artifacts/steam-font-contract/font-candidate-signed.hap` | `017cf19672754f161921e0976fa6e21976307f1554d78ccb346b22a94bdde3d9` |
| 最终 payload | `b651fac62b107e0235fd06c2da8ea977ecde862c766cad0111f109a8c4269d1e` |
| 候选 win32u.so | `37fdcaae2b6aa318440af5ca214d899553bc5a93e44820d6dded55e0eff4cf91` |
| 原 FEX / 实机 libarm64ecfex.dll | `8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc` |
| 已恢复 AMD64 字体测试 | `7f145e4c59db109b62688e26154a1451227da50071673e541f1fd8143006529b` |
| 已恢复原 contract-amd64.exe | `e4ec04f80a2d44076515f0d13369b904192879a90db45e6d2c6ed047a69d3f50` |

上传 HAP、FEX、测试的设备哈希已核验，payload 与设备 manifest 对上。外部 shell 无法独立读取 HAP native win32u.so；上表该哈希来自已安装包，不能伪称读了运行中映像。首次字体对照包与最终包只差后加的测试 suite/summary 能力，win32u/FEX 相同。

已核验客户端参考身份（可能被 Steam 自动更新，下一轮先复核）：

- Steam buildid `1788652215`，CEF `126.0.6478.183`。
- steamwebhelper.exe：`f9ee1d1cc0f06fad16c9a277136a2e1b00b6b668901e28454a7dd8af29afde0d`。
- libcef.dll：`c85bf94462b3c79baeac63823db52d15e116eead71ea414e5d3a112c59e434d3`。
- dwrite.dll：`56ee1afc90078943dec66c8a6140ad56d872f189b4df650c5185dc7373c7ca16`。
- 旧定位 RVA 只用于同哈希二进制：steamwebhelper `AfterCreated=0x131950`、日志返回 `0x1319e1`、SetName `0x1493f0`。不要把它们当跨版本稳定符号。

## 6. 脏改动与不可误带的候选

主树和 Wine 独立树都已有大量未提交修改，**不要 reset/clean 或整体回滚**。当前 HEAD 不能代表平板运行版本。

- 主树已脏：zc_bridge.cpp/.h、wayland_server.cpp、xdg_shell.cpp、wine_child.cpp、GameHook.ets、assemble/build/check 脚本等。
- FEX 源码在 `build/fex-src`，不是正常主树版本化源码。
- **`build/fex-ec/Bin/libarm64ecfex.dll` 曾是诊断版本**，哈希 `89297b45b4fabd49226d6ce968f59177bd07e3d8493f0929d42472705d425c1d`，不是上述实机原 FEX。重新 assemble 会复制它；先核验并显式选择，不要无意带回。
- `scripts/build_fex.sh` 已改，会叠加诊断/ThreadTerm 候选；不要为字体/输入调查顺手执行。
- FEX ThreadTerm 两个行为补丁、各种 boundary/deep/recursion trace 补丁都在 `scripts/patches/`。关闭日志开关不等于撤销行为补丁。
- ntdll 有 TEMP-DIAG 的 NULL 线程入口改 RtlExitUserThread(0)，会吞异常，待独立审计，不属于本次字体修复。
- 保留已证实的导入、字体/936、FEX 缓存、必要栈/子进程后端/异常修复；只对无证据行为改动做独立对照，不要全部撤销。
- 旧字体启动代码会直接改 .reg；其备份曾是空文件、坏字体判定范围过宽。这是独立待审计问题，不要与当前消息停滞一锅改。

## 7. 可复用命令与操作陷阱

以下为 PowerShell 调用。命令中的路径、PID、日期先核验再用。

```powershell
# 有界、只读采集；hilog -x 与 -z 不可组合
hdc -t 5KPBB25818203996 shell date
hdc -t 5KPBB25818203996 shell 'ps -ef'
hdc -t 5KPBB25818203996 shell hilog -x -P 30473 > F:\WineHua\device-evidence-20260920-steam-review\next-hilog.log
hdc -t 5KPBB25818203996 shell 'tail -n 100 /data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_20260920.log'
hdc -t 5KPBB25818203996 shell uitest screenCap -p /data/local/tmp/next-steam.png
hdc -t 5KPBB25818203996 file recv /data/local/tmp/next-steam.png F:\WineHua\device-evidence-20260920-steam-review\next-steam.png

# 只有需要新一轮时才启动；用户未要求此次交接再重启
hdc -t 5KPBB25818203996 shell aa start -b app.hackeris.winehua -a EntryAbility --ps winehua.mode game --ps winehua.game_path C%3A%5CProgram%20Files%20%28x86%29%5CSteam%5Csteam.exe --ps winehua.container_id default --ps winehua.d3d_env_count 1 --ps winehua.d3d_env_key0 WINEHUA_DIAG_QUIET --ps winehua.d3d_env_value0 1

# 非破坏复用 prefix 的字体 suite，运行前引擎须完成更新
hdc -t 5KPBB25818203996 shell aa start -b app.hackeris.winehua -a EntryAbility --ps winehua.mode smoke --ps winehua.suite steam-font --ps winehua.run_id NEW_UNIQUE_RUN_ID --ps winehua.prefix reuse

# Wine 字体模块局部编译，避免全量带入其他未验证修改
wsl -d Ubuntu-22.04 -- docker exec -w /data/src/winehua/build/wine-ohos-aarch64 wineohos-build make -j8 dlls/win32u/win32u.so
wsl -d Ubuntu-22.04 -- docker exec -w /data/src/winehua wineohos-build bash scripts/build_steam_font_contract.sh

# 原生 ARM64 消息响应探针，无需修改 FEX
wsl -d Ubuntu-22.04 -- docker exec -w /data/src/winehua wineohos-build /data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin/aarch64-w64-mingw32-clang -O2 tools/steam-boundary/window-response.c -luser32 -o artifacts/steam-font-contract/window-response-arm64.exe

# 隔离候选；构建前确认 baseline.hap 和目标模块身份
wsl -d Ubuntu-22.04 -- docker exec -w /data/src/winehua wineohos-build python3 scripts/package_font_candidate.py

# 完整常规流程，仅在审计过 staging/FEX 和脏改动后执行
wsl -d Ubuntu-22.04 -- docker exec -w /data/src/winehua wineohos-build bash scripts/w1-m2-assemble.sh
wsl -d Ubuntu-22.04 -- docker exec -w /data/src/winehua wineohos-build bash scripts/w1-m3-build-hap.sh

# Git 经 WSL，坏的 dxvk 子模块元数据要求忽略 submodules
wsl -d Ubuntu-22.04 --cd /home/liufeng/src/WineHua-proton-ohos -- git status --short --ignore-submodules=all
wsl -d Ubuntu-22.04 --cd /home/liufeng/src/WineHua-proton-ohos -- git diff --check --ignore-submodules=all
```

签名须在容器中 `export TOOL_HOME=/apps/harmony; source scripts/env.sh` 后执行 `python3 sign.py INPUT OUTPUT`。**sign.py 会打印含签名口令的命令**，输出只能重定向到本地私有日志，不要回显/粘贴/提交该日志；只检查输出存在并运行 `python3 scripts/check_runtime_components.py --hap OUTPUT`。该脚本调用 os.system，签名失败未必可靠返回错误码，必须检查新产物身份。

覆盖安装：上传到 `/data/local/tmp/...hap`，`hdc ... shell bm install -p ... -r`。不卸载、不清 prefix。更新后可能显示“应用引擎更新（保留数据）”；仅安装不等于运行时已解包，必须核对 `files/wine/.winehua-runtime-manifest.json`。game 模式会驱动更新，smoke 请求在未就绪时可能不执行。初始化期间旧结果文件仍存在，不要读到旧 PASS/FAIL 就算新轮完成。

权限和命令坑：

- hdc shell UID2000；`shell -b app...` 不等于 app UID。通常不能在 app 内新建文件，覆盖已存在可写文件可成功。
- App 经 Z: 访问 `/data/local/tmp` 失败；别反复用该目录作可执行入口。HAP native so 不能这样热替换。
- 前面临时借用过 `C:\smoke\x64-fex\font-contract-amd64.exe` 运行 ARM64 响应探针，**交接前已恢复并校验哈希**。若再次借路径，务必备份/恢复，不能把“x64”文件名当架构证据。
- 本轮 `cmd copy Z:\data\storage\el1\bundle\libs\arm64\win32u.so ...` 失败，没取到实机 native 文件；别把它写成成功。
- 游戏参数用 `winehua.game_argc` + URI 编码的 `winehua.game_arg_encN`；环境走 Want 白名单，别假设任意键有效。
- PowerShell 对远端 `|` 和引号易拆分，优先完整单引号远端命令，或把输出保存后本地筛选。避免不退出的 hilog 管道，设备已有历史残留。
- Docker 产物属 root 时 Windows Copy-Item 可能拒绝；用 WSL root 的明确路径 cp，别反复重试相同操作。
- 截图原始 2560×1600，Wine 桌面 1280×800；图像预览可能缩小。坐标用原图尺寸与日志校验，别按聊天预览尺寸盲点。

## 8. 证据索引与阅读顺序

1. 本文：当前交接入口。
2. `docs/steam-win64/ROOT_CAUSE_REVIEW_20260920.md`：原理、反汇编与完整事实，**第六/七节更新晚于开头只读阶段结论**。
3. `docs/steam-win64/STAGE2_WINDOW_WEBHELPER_20260920.md`：此前窗口/renderer 排查；历史假设以最新证据修正。
4. `docs/steam-win64/STEAM_WIN64_HANDOFF_20260919.md` 和用户 Downloads 中旧指南：背景，不作为当前待执行指令。

私有证据目录中的重点文件：

| 文件/前缀 | 用途 |
| --- | --- |
| `font-contract-amd64-before.jsonl` / `font-contract-*-after.jsonl` | 字体前后对照 |
| `font-fix-20260920/` | 最终套件 PASS 汇总及两架构结果 |
| `font-runtime-after.log` | I386 实际后端等运行证据 |
| `font-steam-{webhelper,cef,runtime,hilog}.log` | 首轮字体修复后 Steam 证据 |
| `font-steam.png` / `font-main-late.png` | 登录 HTML 与后续黑屏 |
| `steam-input-{before,library-click,friends-click,start,probe}.log` | 最新输入链、窗口身份、旧世代退出 |
| `steam-input-html.log` | 15:44:44 自发 IPC timeout 重启 |
| `window-response-{during,after}-restart.txt` | 新 PID 2988 消息超时；不是旧 PID 栈 |
| `steam-input-current.png` / `steam-input-start-late.png` / `steam-input-restarted.png` | 主框架、可响应的开始菜单、重启后画面 |
| `font-recursion-*.log/json`、`cef-*-audit.asm` | 原字体递归证据 |
| `system-reg-font-*.reg`、`user-reg-font-audit.reg` | 字体表状态，含私密机器/账户信息，不提交 |
| `winehua-steam-occlusion-baseline.hap` | 隔离字体修复的基线包 |
| `libarm64ecfex-original.dll` | 原 FEX 留存 |

## 9. 对接手 AI 的直接要求

请先复核现场世代再行动。把“已观察到”“源码推断”“待验证”明确分开。当前最有价值的工作是定位 browser UI 消息停滞的第一个缺失边界，并用独立双窗口测试判定合成缺陷；不要重复字体试错、扩大栈、切 GPU 参数和无依据修改 IPC。

本交接是任务材料，不覆盖用户的新要求。遵循当前会话的 AGENTS/工具权限；当前会话没有授权随意发消息给外部人员或上传私有证据。修改保留可重现补丁、实际加载身份和同轮验证结果，未通过库页/交互/重启不能报“Steam 已修好”。
