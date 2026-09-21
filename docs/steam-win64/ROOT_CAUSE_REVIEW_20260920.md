# Steam 64 位故障复核：字体初始化递归与字体注册不一致

> 最新交接入口：[AI_HANDOFF_20260920_FONT_INPUT_WINDOWS.md](AI_HANDOFF_20260920_FONT_INPUT_WINDOWS.md)。本文前五节保留只读调查时点，第六节是已部署字体修复和验证，第七节是后续输入/多窗口调查；请勿把前文“尚未执行”当成当前状态。

2026-09-20。承接 `STAGE2_WINDOW_WEBHELPER_20260920.md`。本轮按用户要求暂停启动和参数试错，分析已有调用记录、固定二进制、对应上游源码，并读取已停止 App 的字体状态；没有部署或修改平板运行时。

## 结论与边界

**最新证据将 browser 初始化失败的优先修复点收敛到字体发现/注册链。** 保存的现场出现“字体创建失败 → 默认字体初始化 → 再次创建字体”的递归；当前 prefix 也存在足以使 DirectWrite 缺少普通 UI 字体的注册不一致。此前把反复调用笼统解释为 FEX 回调不返回、引用计数递归或 IPC 丢通知，均应修正。

- **已确认的运行机制**：探针现场中，CEF 进入字体创建失败分支后重新初始化系统字体；反复进入同一调用点时，RSP 每层减少 `0x700`（1792 字节）。这是一条明确的业务递归链，不能再因为出现 dwrite exit thunk 就推断 ARM64EC 返回栈错误。
- **已确认的当前环境缺陷**：HKCU 外部字体记录与 HKLM 中 DirectWrite 使用的字体表不一致。20 个系统字体值指向空的 Windows Fonts 目录；实际存在的鸿蒙黑体、HarmonyOS Sans 等没有登记到该 HKLM 表。
- **尚待闭环**：没有保存 PID 9174 初始化瞬间的注册表，也没有完成仅修复字体注册后的原 FEX 对照。不能声称全部原始 SIGSEGV 都已解释，更不能宣称主画面修好。字体表最初被谁、何时改坏，尚无直接证据。

renderer/GPU 崩溃和 ANGLE 顶点缓冲错误属于已记录的后一阶段问题，不能与 browser 的字体初始化失败混成一个故障。

## 一、递归链的确定依据

| 文件 | 身份 |
| --- | --- |
| steamwebhelper.exe | SHA256 `f9ee1d1cc0f06fad16c9a277136a2e1b00b6b668901e28454a7dd8af29afde0d` |
| libcef.dll | SHA256 `c85bf94462b3c79baeac63823db52d15e116eead71ea414e5d3a112c59e434d3`；文件版本 `126.0.0-HEAD.3171+gbbb7596+chromium-126.0.6478.183` |
| 平板 dwrite.dll | SHA256 `56ee1afc90078943dec66c8a6140ad56d872f189b4df650c5185dc7373c7ca16`；runtime 和 prefix system32 副本相同 |
| dwrite 有效 .text | 按 section VirtualSize 计算 SHA256 `121f532cc5910f1f61a48f00b1d1880640f1cb48cd9094ba6bf766a562dfd883`，与本地带符号产物一致；完整文件及 raw padding 不同 |

现场为宿主 browser PID 9174、Windows PID 696、Windows TID 700。libcef 基址 `0x6fe7c90000`，dwrite 基址 `0x6fe7ab0000`。该轮使用临时全调用探针和 32 MiB 栈，最终为 `c0000354`/Quit 终止；它不是原 DLL 的首个 SIGSEGV 现场。

将实际二进制反汇编与 Chromium `126.0.6478.183` 源码匹配得到以下链。CEF 名称是源码/反汇编匹配，**不是 PDB 符号化**；dwrite 名称来自有效代码段一致的带符号产物。

```text
AfterCreated（steamwebhelper RVA 0x131950）
  → 0x126ae0：User-Agent 构造完成，之后继续调用 CEF
  → 系统字体初始化 / AddFont
  → GetFontFromLOGFONT → PlatformFontSkia 字体创建
  → InitFromDetails → CreateSkTypeface
  → Skia/DirectWrite：FindFamilyName、MapCharacters
  → 请求字体及 "sans" 回退均未生成 typeface
  → InitFromDetails 的 !success 分支
  → EnsuresDefaultFontIsInitialized
  → GetDefaultSystemFont → 尚未完成的系统字体初始化
  → 再次 AddFont……
```

| libcef RVA | 源码匹配 / 二进制证据 |
| --- | --- |
| `0x158e840` | `SystemFonts::AddFont` |
| `0x158e500` | `GetFontFromLOGFONT`，调用 GDI 后构造字体 |
| `0x1a01bb0` | `PlatformFont::CreateFromNameAndSize` |
| `0x1a01050` | `PlatformFontSkia::InitFromDetails`；`0x1a010fa` 检查 success，失败跳转 `0x1a011fc` |
| `0x1a01270` | `CreateSkTypeface`；首选失败再试常量 `sans`，第二次失败写 `success=false` |
| `0x1a011fc` | 直接调用 `0x1a00910`，返回地址 `0x1a01201` |
| `0x1a00910` | `EnsuresDefaultFontIsInitialized`，内部调用系统字体获取 |
| `0x158e1c0` | 系统字体获取及尚未完成时的初始化 |

TID 700 的两段连续记录为 seq 5480–5580、5929–6029，各 101 条，段内无序号缺失。第一段 `CreateSkTypeface` 在 seq 5578 返回至 `0x1a0119b`，紧接 seq 5579 调用 `0x1a00910`，caller 为 `0x1a01201`；下一段重复同一路径。这与反汇编中的 **success=false** 分支对应。

在 seq 5000–13999 的深度窗口内，`AddFont` 出现 20 次，19 个相邻 RSP 差值全部为 `0x700`；`InitFromDetails`、`CreateSkTypeface` 和默认字体重新初始化有相同差值。外层字体初始化未完成就重新进入；不是某个引用释放包装函数自身不返回。全局 seq 是筛选后的计数，不能当作所有调用总数。

| dwrite RVA | 带符号函数 |
| --- | --- |
| `0x60c94` | `dwritefontcollection_FindFamilyName` |
| `0x800d8` | `dwritefactory_CreateNumberSubstitution` |
| `0x53360` | `fontfallback_MapCharacters` |
| `0x9120c` | 四参数 ARM64EC exit thunk 的回调返回点 |

`MapCharacters` 回调之后确实回到 CEF 的清理和返回路径。下一步应取得其 HRESULT、mapped length、输出字体和实际 family；不能把正常跨架构回调本身当作错误。

上游代码解释了该循环：`InitFromDetails` 在字体创建失败时调用 `EnsuresDefaultFontIsInitialized`；Windows 默认字体又通过 `SystemFonts::Initialize` 创建，而初始化完成标志在所有 `AddFont` 之后才设置。应先恢复其依赖的字体可用性，无须先修改闭源 Steam/CEF 二进制。

## 二、字体文件存在，但 DirectWrite 看不到

从已停止 App 的 prefix 读取 system.reg/user.reg，并列出文件目录：

| 检查项 | 结果 |
| --- | --- |
| HKCU `Software\Wine\Fonts\External Fonts` | 240 个值，包含系统字体及 runtime 中的 Tahoma 等 |
| HKLM `Software\Microsoft\Windows NT\CurrentVersion\Fonts` | 22 个值；20 个为 `arial.ttf`、`tahoma.ttf` 等相对文件名，其余两个为普通/竖排 `HarmonyOS Sans Digit` |
| HKLM Win9x 对应 Fonts 表 | 仅两个 Digit 值 |
| HKCU 外部字体中 HKLM NT 缺少的同名值 | 232 个；另有 6 个同名值存在，但路径仍为旧相对文件名 |
| prefix `drive_c/windows/Fonts` | 空目录 |
| runtime `share/wine/fonts` | 13 个 TTF，包含 `tahoma.ttf`、`tahomabd.ttf` |
| `/system/fonts/HarmonyOS_Sans_SC.ttf`、`HarmonyOS_Sans.ttf` | 文件存在；前者对应替换链使用的“鸿蒙黑体” |

Wine `dlls/dwrite/main.c::create_system_path_list()` 从 **HKLM NT Fonts** 构建 DirectWrite 系统集合。没有反斜杠的值会被拼接到 Windows Fonts 目录；它不会因为 HKCU 的 GDI 外部字体记录完整就自动从那里读字体。

所以“GDI 能显示桌面”“字体文件存在”“字体替换表已修过”均不能证明 DirectWrite 字体集合可用。20 个相对路径确实指向空目录，普通 UI 字体没有登记，构成直接需要修复的环境缺陷。不声称 Digit 的字符覆盖已验证：本轮只核对文件存在，未成功读取 cmap。

还有一个阻止自愈的具体源码条件：`win32u/font.c::update_external_font_keys()` 在 HKCU 记录与已扫描 face 路径相符时，直接设置 `ADDFONT_EXTERNAL_FOUND`，随后跳过两个 HKLM Fonts 表的写入。它没有检查目标 HKLM 值是否仍存在、路径是否正确。**因此“HKCU 完整、HKLM 缺失”可以在后续初始化中继续保留。** 这是持久化缺陷的解释，不能当作首次丢失来源的证明。

## 三、具体修改和验收顺序

### 1. 修复字体注册一致性

优先修改 Wine `update_external_font_keys()`，或通过 Windows 注册表 API 执行一次受控迁移：

1. 对已成功枚举、文件真实可用的外部字体，核对 HKCU、HKLM NT、HKLM Win9x 三处值。只有目标表内容也正确时，才能进入 `ADDFONT_EXTERNAL_FOUND` 跳过路径。
2. 缺项或已确认指向不存在文件的旧相对路径，补入规范、可打开的实际路径；保留合法用户字体。不要按名称包含 `HarmonyOS` / `Noto` 就判定失效。
3. 确保 runtime 自带 Tahoma 等能被 DirectWrite 发现；打包有 13 个 TTF 不是验收。注册必须在 browser 创建字体集合前完成，并在冷启动后仍一致。
4. 保留中文代码页及真实 family 替换修复，增加 DirectWrite 集合验证，不能用 GDI 字体断言消失代替。

避免为此继续在每个子进程中直接截断写 `.reg` 文件。当前 `ensure_prefix_fonts_and_codepage()` 位于 Wine 子进程启动路径，不能仅凭“本子进程尚未进入 Wine”保证整个 wineserver 会话已停止。另其 `.winehua.bak` 只创建空文件；设备两份备份均为 0 字节，不能恢复。迁移应由单一会话启动点执行，控制注册表写入时机，并生成真实备份。这些是源码审计发现，不是已证明的字体丢失来源。

### 2. 用实际失败的字体契约验收

同源构建 I386/AMD64 小程序并核验 PE Machine，先对损坏状态记录结果，再只修字体注册对照：

- `GetSystemFontCollection`、`FindFamilyName`：记录 family、HRESULT；检查真实鸿蒙黑体/Tahoma，以及 CEF 实际请求的名字。
- native Wine `IDWriteFontFallback::MapCharacters` 回调 AMD64 `IDWriteTextAnalysisSource`；检查空格、拉丁字母、中文的 HRESULT、mapped length、font、scale，以及 font face 创建。
- 同线程多次往返，检查回调参数、返回与栈平衡；区分“字体为空”和 ABI 错误。
- 冷启动后再次检查集合，证明没有被后续启动覆盖。

此测试先于完整 Steam 重启。已有事件、共享内存和线程测试不覆盖这条字体 COM 链；`--skip-terminate` 的总体 PASS 也不能当作完整终止测试通过。

### 3. 固定条件，只做字体修复对照，再处理显示

原 FEX、关闭全调用探针、同一 Steam/profile/启动参数，只引入字体修复，观察同一世代 `AfterCreated → BrowserReady → SetName → renderer` 180 秒。

| 结果 | 下一步 |
| --- | --- |
| 字体测试恢复、递归消失且握手恢复 | 根因得到对照验证；继续检查冷启动持久性 |
| 字体集合正常但 MapCharacters 失败 | 依据 HRESULT/输出定位 dwrite、别名或文件打开；回调契约确实错误才改 ARM64EC/FEX |
| 字体契约通过、Steam 仍进入字体失败分支 | 只采该轮 family、HRESULT、font 和几个指定边界，解决差异，不开全调用日志 |
| 握手稳定但主画面黑 | 定位 ANGLE D3D11 的 buffer size、stride、offset、vertex/index range 和首次 renderer/GPU 异常，再检查主/弹窗呈现 |

最终仍需关闭新增诊断后的三次冷启动、实际 HTML、输入/切页、登录库页和退出重开。目前没有这些 PASS。

## 四、之前改动应如何处理

| 改动 | 审计判断 |
| --- | --- |
| steamclient64 导入重定向 | 有明确错误与修复依据，保留；不能解释当前递归 |
| 字体真实 family / 936 | 保留意图，补齐 DirectWrite 注册；原“字体链整体健康”的结论过宽 |
| dwrite `IID_IDWriteFont3` 对齐 | 与 `HasCharacter` 接口类型一致，不应无依据回滚 |
| FEX cache、已证实 ABI/栈/子进程后端修复 | 保留；扩大栈不能解决无界递归 |
| 全调用及多轮 recursion 探针 | 隔离为诊断构建；正常边界仍格式化/输出，已经改变终止时序 |
| 两个 FEX ThreadTerm 补丁 | 独立候选行为修复；关闭 trace 不会撤销它们，不算字体修复 |
| ntdll NULL 线程入口改为 `RtlExitUserThread(0)` | 标为 TEMP-DIAG 但实际吞掉异常，应在独立比较构建中移除/审计 |
| 同步等待修改 | 已阅 diff 主要为诊断，未发现 auto-reset 核心语义变更；没有证据支持改 IPC 语义 |
| xdg role 生命周期修复 | 真实独立缺陷，单独验收，不能解释字体递归 |
| 全零 SHM 遮挡绕过 | 未证明恢复主画面；当前源码按 FrameSerial 缓存扫描，旧“每帧全扫”描述已过时 |

不要整体回滚；也不要把诊断和未验证行为补丁混入所谓“原版”基线。后续记录具体补丁清单与实际 DLL 哈希。

## 五、32/64 差别、曾经启动成功与反复试验

I386 WoW64 与 AMD64 ARM64EC/FEX 的 DLL、回调、异常和后端确实不同，旧客户端/CEF 版本也不同。但**本次字体初始化机制不是 64 位独有**，不能用“64 位更复杂”代替根因。

旧 32 位到界面只证明当时客户端、字体状态和调用路径能运行。当前 Wow6432Node Fonts 是指向主字体表的注册表链接，也不能假设 32 位拥有独立健康的字体表。

重装后曾有 BrowserReady、renderer、登录和主窗口帧，说明架构路线可行，也否定“64 位从未能启动”的概括。当前字体表不一致及源码无法补回缺项，解释了一个可持续存在的失败条件；但尚不能确定它发生于安装、wineboot 更新、文件写入竞争还是其他动作，不能断言“全是安装不完整”。

此前低效在于：混合启动与显示阶段、版本/profile 未固定、把等待入口当死锁、把 ARM64 smoke/x64 当 AMD64、诊断改变运行时序。本轮用具体失败分支及调用循环决定修复位置，不再排列组合 GPU/栈参数。

## 证据与本轮验证范围

私有材料在 `F:\WineHua\device-evidence-20260920-steam-review\`。原始注册表可能含账户/机器信息，不提交仓库。

- `boundary-verbose-9174-maps.txt`、`boundary-9174-tid700-audit.log`：同进程映射和过滤记录。
- `font-recursion-two-cycles.log`、`font-recursion-summary.json`、`font-recursion-edges.json`：连续窗口和栈差统计。
- `cef-font-init-audit.asm`、`cef-system-font-audit.asm`、`cef-dwrite-fallback-audit.asm`：固定 CEF 反汇编。
- `font-registry-extract.txt`、`font-external-registry-extract.txt`、`font-registration-summary.json`：字体注册对照。
- `dwrite-device.dll`：有效 .text 与本地符号产物一致。
- `cef-font-reference/126-*`：对应版本源码；无 `126-` 前缀的 main 文件仅作检索参考，不作为版本对应依据。

上游依据：

- [Chromium 126 platform_font_skia.cc](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/gfx/platform_font_skia.cc)
- [Chromium 126 system_fonts_win.cc](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/gfx/system_fonts_win.cc)
- [该版本 DEPS](https://github.com/chromium/chromium/blob/126.0.6478.183/DEPS) 固定 Skia `be621ea04206d8fae23952783d1d588d6ce0d9b3`。
- [对应 SkFontMgr_win_dw.cpp](https://github.com/google/skia/blob/be621ea04206d8fae23952783d1d588d6ce0d9b3/src/ports/SkFontMgr_win_dw.cpp)

上述为只读调查阶段的边界；随后实施和实机验证如下。

## 六、字体修复实施与实机对照（2026-09-20 下午）

### 产品化范围

旧 `ensure_prefix_fonts_and_codepage()` 已将字体 family 替换和 936 配置写入产品，但没有保证 DirectWrite 的 HKLM Fonts 表与 GDI 的 HKCU External Fonts 一致。故不能说旧字体修复完全未产品化，也不能说已覆盖全部字体链。

本次在 Wine `dlls/win32u/font.c::update_external_font_keys()` 修复：即使 HKCU 与已扫描字体相符，也检查 NT/Win9x 系统表；补缺项、修复指向不存在文件的路径，保留仍有效的注册。I386 和 AMD64 共用此初始化层。未硬编码平板 UID 或某一 Steam 版本。

- 补丁：`patches/wine/0001-win32u-repair-external-font-registration.patch`。
- `scripts/build_wine.sh` 幂等应用该补丁；刷新源码后仍可重建。
- `tools/steam-boundary/font-contract.cpp` 同源生成真实 I386/AMD64 测试。
- `assemble.sh` 打包测试及 SHA256，提供 `steam-font` suite；完整性检查校验文件哈希、suite 和 PE Machine。
- 为隔离已有脏改动，`package_font_candidate.py` 从保存的基线 HAP 制作候选，仅替换 win32u.so 与测试/清单；其他 HAP 条目逐一比较不变。候选保持原 FEX，没有加入 ThreadTerm 候选补丁。

### 实测结果

| 项目 | 原运行时 | 字体修复后 |
| --- | --- | --- |
| AMD64 DirectWrite 字体家族数 | 1 | 226 |
| Tahoma / 鸿蒙拉丁 / 鸿蒙中文 / sans fallback | 四项全部 FAIL | 四项全部 PASS |
| AMD64 字体 COM 回调 | 失败后终止，12 次 | 512 次，正常退出 |
| I386 同源测试 | 本轮未收集原运行时基线 | 226 家族、四项 PASS、512 次回调 |

AMD64 实际后端为 libarm64ecfex，I386 为 wowbox64；不是拿 ARM64 PE 冒充 AMD64。测试不宣称直接验证寄存器或全部栈平衡。临时覆盖的 `contract-amd64.exe` 已恢复。

Steam 对照轮：15:27:21 冷启动，CEF 126.0.6478.183，buildid 1788652215，browser Windows PID 608。15:27:46 `AfterCreated`，15:27:47 同轮 `SetName`、`BrowserReady` 和 renderer 请求，实际 renderer 宿主 PID 28031；15:28 登录 HTML 可见，中文和背景图正常。15:29 自动登录推进，15:30 创建 Steam 主窗口。观察超过 180 秒，初始化握手没有退回原卡点。

**剩余故障仍存在：主窗口随后黑屏，未通过库页/交互/三次冷启动验收。** 15:32:29 主窗口 `(28031,44)` / toplevel 8 已绑定 producer `0x6da000000022`，累计 15838 帧，producerAgeMs=5、drawAgeMs=5。说明提交和消费继续，不能把它再归因为没有 renderer 或完全未绑定窗口。下一步应对同一帧检查源像素与最终合成，而不是继续修改字体、扩大栈或凭等待日志修改 IPC。朋友列表的 `ProducerDead` 分类仅表示帧龄阈值超限，不等同于已证实进程死亡。

证据目录 `device-evidence-20260920-steam-review`：`font-contract-*-before/after.jsonl`、`font-runtime-after.log`、`font-steam-webhelper.log`、`font-steam-cef.log`、`font-steam-runtime.log`、`font-steam-hilog.log`、`font-steam.png`、`font-main-late.png`。包含私人运行材料，不提交原始账户日志。

### 产物与检查

- win32u.so SHA256：`37fdcaae2b6aa318440af5ca214d899553bc5a93e44820d6dded55e0eff4cf91`。
- 原 FEX 实机 SHA256：`8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc`，保持不变。
- 最终带 suite 的签名候选：`artifacts/steam-font-contract/font-candidate-signed.hap`，SHA256 `017cf19672754f161921e0976fa6e21976307f1554d78ccb346b22a94bdde3d9`；设备上传包哈希相同，覆盖安装成功。
- Wine 字体模块编译、I386/AMD64 编译、HAP runtime closure、shell 语法与 diff whitespace 检查通过。
- 首轮字体/Steam 对照使用同一 win32u 与原 FEX，但最终包随后补充测试 suite/汇总输出，须区分包身份；本次未全量重编 App/native 图形模块。
- 最终包实机 `font-fix-20260920` suite：整体 PASS；I386 宿主 PID 32436、AMD64 宿主 PID 32446 均 PASS。汇总和两份结果已收集到私有证据目录同名子目录。最终运行时 payload SHA256 `b651fac62b107e0235fd06c2da8ea977ecde862c766cad0111f109a8c4269d1e` 已与设备解包 manifest 对上。
- 设备外部 shell 无权直接读取 HAP native 库，因此 win32u 的 SHA256 来自已安装候选 HAP，未伪称已独立读取运行中模块哈希。FEX 和上传 HAP 的设备哈希可读取并已对上。
- 原 HKLM NT Fonts 表 22 项，修复会话后落盘 254 项；原有效 HarmonyOS Sans Digit 路径保持不变。磁盘快照中仍有 Tahoma 相对路径，不能仅凭登记数量声称所有失效路径均已清除；当前实际 DirectWrite 查找/建 face 和 fallback 用例均通过。任意自定义字体路径保护尚未专门注入测试。

## 七、输入无响应与好友列表异常（15:42—15:46 同会话调查）

本阶段不重启 App、不修改字体/FEX/GPU 参数。Steam 自行重启 webhelper 必须单独标记世代。

1. 当前主画面可显示 Steam 顶栏、导航和底栏，中间网页仍黑。15:42:49 实机点击物理 `(280,94)`（库），换算 Wine `(140,47)`，命中主窗口 toplevel 6，origin `(0,0)`、scale 1；pointer enter、按下、抬起均 `sent=1`。这里可证明发送到了对应 Wayland 客户端，不能据此证明 Wine 已消费或 CEF 已执行。
2. 主窗口 `(33090,32)` 与 inputTarget=6 对上。好友列表 `(33090,43)` / toplevel 8 / producer `0x816500000022` 是独立窗口，不能把它称为已证实独立进程；15:44:03 帧龄 223422 ms。`ProducerDead` 只是帧龄分类，不是进程死亡证明。
3. 点击 Wine 开始按钮后，Shell_TrayWnd 收到输入、提交更新，开始菜单最终可见。证明本会话鸿蒙→Wayland→Wine 输入和 SHM 菜单显示没有整体失效；Steam 特定窗口路由仍不能完全排除。
4. **15:44:44 Steam 明确记录 `Failure pushing command to webhelper 0/16704`、`PushCommand: Restarting webhelper process due to IPC timeout`，关闭 Windows PID 1044，启动 PID 2988。** 这是持续无响应的独立证据，不能仅用坐标偏移解释。15:44:56 原 browser 宿主 PID 33090 和相关子进程退出、toplevel 6/8 销毁。退出期间 GPU/renderer 的 SIGSEGV 不足以证明它们是超时的首因，不能倒置时序。
5. 新增原生 ARM64 `window-response.c`，只枚举可见窗口并对 WM_NULL 作 500 ms SendMessageTimeout，不发送应用命令。**采样发生于自动重启后的 PID 2988，不是之前 PID 1044 的线程样本。** 两次采样 Steam `SDL_app` HWND `0x2014e`、TID 2992 返回 ERROR_TIMEOUT=1460（首次 501 ms，后次因 ABORTIFHUNG 立即拒绝）；同会话 shell/菜单正常返回。说明新世代窗口消息处理也不健康，尚未定位阻塞调用。

合成层审计发现需要独立验收的限制：`EglRenderer` 每实例维护单个 `zeroCopySurfaceKey_`，已注册时继续使用该源；桌面绘制先画 CPU 桌面，再画一个 external-OES 层并回填遮挡区。仅“绑定成功/帧计数增长”不能证明主窗口、好友窗口等多个 native 画面都正确参与合成。好友窗口显示异常可能包含该限制，但不能从本次停帧直接断言是唯一根因。

下一步按证据顺序：

- 首先在同一 browser 世代记录 Wayland 接收、Windows 窗口线程取消息、CEF 消费三个边界；与 WM_NULL 超时关联，取得 UI 线程阻塞位置。不要仅凭 Steam IPC timeout 就修改事件同步语义。
- 单独构造两个 native 图形窗口验证桌面合成：前后切换、静态好友窗口保持最后一帧、遮挡、点击目标一致。若证实只绘制单个 producer，改为每层保留纹理并按共同 z-order 合成；不能靠定时更换“最新 producer”修正。
- 对主网页黑区取源 buffer 像素与最终画面做对照。持续提交相同/空白帧并不代表 renderer 正常执行页面任务。

证据：`steam-input-{before,library-click,friends-click,start,probe}.log`、`steam-input-html.log`、`window-response-{during,after}-restart.txt` 及同名前缀截图，位于私有设备证据目录。探针曾临时占用已存在的字体测试路径，结束后已恢复真实 AMD64 字体测试。未部署新的行为修复。
