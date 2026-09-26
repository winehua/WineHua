# 定制点三问审计（必要性 / 合理性 / 可验证性）

适用场景：重建式升级时决定每个定制点「带走/改造/丢弃」，规划自动化验证建设时看这篇。
逐点明细与分类口径见 `docs/assets/module-inventory.md`（四分类 A/B/C/D）。
最后核实：2026-09-26（11 组并行审计，约 130 个产品定制点逐一回答三问）。

三问定义：① 必要性——还需要吗，依据什么平台约束；② 方案合理性——现做法是否合理，有无更优做法；③ 可验证性——如何用自动化手段检验其有效与失效。

## 总体结论

1. **必要性**：绝大多数定制点对应真实平台硬约束（沙箱无 execve/fork、noexec、无 WSI、无 GBM、musl 缺符号、Maleoon 缺能力、Venus shadow 语义），不可删。可删项约 20 个，全部是死代码/被取代/残留/纯清理（清单见下）。大爆炸重写进一步被否定。
2. **合理性**：机制正确的占绝对多数。改方案项约 15 个，每个都有具体更优做法（清单见下）；共性是三类——「特判改机制」（设备特判改能力探测、字面契约改协议下发）、「双实现收单源」（四处）、「启发式改权威信号」（WM_GETMINMAXINFO、foreground 判定）。
3. **可验证性是最大短板**：现有 18 个套件全部落在图形/音频/DNS 且多在 x86_64+shm 路径；输入、UI 层、窗口管理、进程模型、env 管线、宿主 virgl 渲染路径、box64 失效通路七个片区零覆盖或近零覆盖。机制正确的定制「回归全绿证明不了它们没坏」。

## 可删项清单（约 20 个）

| 项 | 位置 | 删除依据 |
|---|---|---|
| Z: 与 HOME 解耦 | wine `3df57f3a046` | 语义已被 ohos_file.c `$HOME` 优先取代 |
| makedep.c install 去重/断言 | wine | 打包全链无 `make install` 调用，触发场景不存在 |
| WGL_FORCE_GLES | wine `ae50645ad47` | 已被下一笔整体删除（代码已不在，仅存档） |
| env 白名单注入 | wine `910056eecb7`+`8ae9f9b62b7` | 已被 `__env__` 全量序列化取代 |
| 重复的扩展索引补丁 | wine `b74b2bd3a81` | 与 `c96fc9d9931` 同补丁 |
| per-exe WINEDEBUG 名称特判 ×3 | wine_child.cpp:94-111 | 修活 WINEHUA_WINEDEBUG 通道后即无存在理由 |
| Heaven pass2 depth ALWAYS quirk | dxvk legacy `5058927a` 系 | 程序特征匹配 hack，自家 trace.h 明示产品不得依赖 |
| desktopLauncherVisible 整链 | WineWindowManager.ets | 写入方零调用、守卫恒 return；须整链删（只删守卫会改变行为） |
| decor 校准残留（EMA/两段校准/1.61 魔数） | WineWindowManager.ets | barH=0 后链内自产自销，不进任何计算 |
| desktopAbilityRunning 死字段 | WineWindowManager.ets | 只写不读 |
| move_end 3000ms dumpWindowLayout | WineWindowManager.ets | 白屏取证残留（2000ms 补定位是功能，保留） |
| 手柄/键盘布局编辑段 ≈200 行 | InputSettingsService.ets | 消费组件 GamepadOverlay/KeyboardOverlay 在 master 不存在，零调用方 |
| PIX-SAMPLE 每 30 帧采样 | egl_renderer.cpp:805 | 与 host 侧 frame.py 视觉判定重叠，常开诊断在渲染循环 |
| NCP-waitpid 死分支 + `virglServerProgramPath_` | graphics_broker.cpp | `usesNcp_` 全库无置位点；字段只打日志不用于启动 |
| 死字段/死谓词 6 处 | compositor（hasSizeLimits、SubsurfaceAsLayer()、ZOrderNeedsParentPosCheck、parentOffset*、CommittedSurface 三字段） | 只写无读者 |
| 墙钟时间戳 | wl_core.cpp:930 | 改 CLOCK_MONOTONIC 后旧实现即删；Wine 只取 done 不比对值 |
| `SendDeleteSurrounding` | text_input.cpp:246 | 无调用方，退格走键盘注入 |
| box64 四笔纯清理提交 | `e5951dd4f` 等 | 无行为变化，平移时丢弃 |
| fullscreenPopup 启发式 | PopupWindowManager.ets | 疑随 InlineClient 退役成死分支，**删前实测确认** |
| glib/gstreamer staged 残留 + pcre2 config.h.in | thirdparty | 构建脚本已负责（patch+幂等 sed），工作树残留应丢弃 |

## 改方案项清单（约 15 个，均有具体更优做法）

| 定制点 | 现状问题 | 更优做法 |
|---|---|---|
| min/max 约束判据 | style 位推断是启发式（修过一次误判） | 改读 `WM_GETMINMAXINFO`——wine 本就知道程序声明的 min/max，消掉整条判据链 |
| fsPriority 全屏仲裁 | 根因在 `win32u/window.c:2339` is_fullscreen 按「矩形盖屏」给所有窗口打标 | winewayland 发 set_fullscreen 前判 `NtUserGetForegroundWindow()==hwnd`，消掉取号/级联两条链 |
| max_size→maximize 启发式 | 前提已被现行 wine 推翻（现在状态变化即发 set_maximized） | 先埋日志实测命中率，零命中即删 |
| 最小化按尺寸猜（>200x50） | xdg 无 unset_minimized 是协议缺口 | 用既有 SC_RESTORE 握手补私有事件 |
| `WINEWAYLAND_ENTER_SILENT` | 宿主固定注入 =1，「排查开关」实为默认路径 | 删 env，改为相对模式下代码默认（带同 surface 守卫） |
| TRANSFER_DST 无条件加 | 不分档位全局生效 | 仅 vkd3d 档（`WINEHUA_D3D_BACKEND` 判定）加 |
| `vkd3d_limited_500k` 字面契约 5 处 | 任一侧改档位名即静默失效 | 宿主直接下发展开后的 overlay 根/专用旗标，wine 不认档名 |
| Z: 盘符双实现 | mountmgr 硬编码 vs ohos_file `$HOME` | 收口 ohos_file 单一出处 |
| ZC 零拷贝页契约 | magic/版本/路径两仓各写一份 | 抽共享头 + 双仓同步检查 |
| guest EGL 预载列表 | 钉死 `libgallium-25.0.1.so` 版本字面量，mesa 升级即 GL 全死 | 目录扫描或读 bundle manifest |
| Maleoon GPU upload 特判 | vendorID+设备名是设备经验非能力判据 | 启动时 mapped-write 可见性探针（写→flush→GPU 读→回读） |
| x86_64 EGL 属性编译特判 | 一刀切且非 OHOS 也降级 | 运行时先全量创建失败再回退最小集 |
| busy-wait+glFinish 兜底 | 宿主无条件下发 `VTEST_SYNC_GL_FINISH=1`，每帧付全管线 glFinish | per-submit GL fence（`eglCreateSyncKHR` 指针已缓存）或至少可关 |
| 四处「同一判据两份实现」 | SetHostShadowProfile 双映射、env_profiles 别名+反查表、phone 线协议无版本、audio 协议头双份+硬编码版本 | 照仓库已有先例收口（header-only 单源、magic/version、契约检查脚本） |
| RELAXED_FEATURES env 放行 | legacy 特有，语义与文档失真 | 照 modern 改设备自动判定，删 env |
| 机型白名单 + perfProfile 写死 | 代码内特判，升级/换机静默走错档 | 迁出为资产表（model+incremental→档位）+ 命中打点；中期用 dxvk26-requirements 探针结果替代 |

## 审计新发现的潜伏 bug 候选

| 项 | 位置 | 说明 |
|---|---|---|
| Z32 深度布局 | virgl `override_formats` | Z32F_S8X24 压到 D24S8：GLES3.0 起有 DEPTH32F 白丢精度；type 8B/px→4B/px，guest 上传/回读布局不匹配，无用例覆盖 |
| sRGB 判定与实现脱节 | virgl `egl_image_srgb_import` | sRGB 导入实现在 `#ifdef ENABLE_GBM` 内（OHOS 无 GBM），判定却按扩展串置 true——判定说能做、实际没做 |
| 墓碑 Set 跨会话污染 | WineWindowManager.ets:112 | 防竞态必要但永不清理；热重启后 wine 重新分配窗口 id 撞旧值，新窗口被静默吞掉 |
| rawDelta 漏乘设备系数 | DesktopWindow.ets:323、WinePopup.ets:322 | 四份手势机两份已修两份未修——同一鼠标两套手感（实锤漂移） |
| modern flush 批处理休眠 | 宿主 modern 档不注入 BATCH env | `bf22f73f` 优化在产品中未生效，应随 `isWineHuaVenus()` 自动开 |
| box64 指针未推默认分支 | box64 origin/master 停在 23750c922 | 主仓指针 0411b3856 只在 feature/core-extract——按默认分支构建无 HTTPS 修复，违反 submodule 硬约束 1 |
| region_subtract 语义做反 | wl_core/wayland_server.h:192 | 「减」做成「加」，靠 Wine 只判空/非空兜底 |
| schannel CHACHA20 禁用 | wine `57bad821` | 无机制正文；HTTPS 可用主因是 gnutls+符号桥，此项必要性待 A/B 实测 |

## 验证缺口矩阵（现有设施覆盖不了的片区）

| 片区 | 现状 | 最小闭环 |
|---|---|---|
| 输入链路 | 14 机制零用例；dinput_probe 半废弃（`make smoke-pe` 目标不存在、assemble.sh 排除） | 救活 dinput_probe 入 smoke；注入用真机 `uitest uiInput`（套件须钉设备类型） |
| UI / ArkTS 层 | automation 全目录无 uitest/dumpLayout | host 侧脚本串 aa start --ps → uitest 注入 → hilog 落盘断言；日志标签/AA 参数/uitest 全现成 |
| 窗口管理协议 | managed/min-max/modal/app_id/toplevel 计数判定依据在宿主收到的 Wayland 请求上，归档无此数据 | 宿主把 winehua-toplevel/xdg 请求摘要写沙箱文件 + `protocol-log` 判定器 + `winehua_win_smoke` 载荷 |
| 进程模型 | 锚点三态/热重启/NCP 退出回调全靠 core 间接压过 | 三个 inline 用例（kill wineserver / explorer 退 / stopAll）+ 归档解析 `state:*` |
| env 管线 | 只有日志打点；SetHostShadowProfile/env_profiles 映射漂移不可测 | env 探针用例（exe 打印 getenv 进 metrics）+ host 判关键键集合 |
| 宿主 virgl 渲染路径 | x86_64 强制 softpipe，Z32/sRGB/shadow/present 在模拟器完全不被执行 | graphics-smoke 结果 JSON 增 transport/zero_copy 字段 + arm64 virgl 档套件条目 + job 协议加 `backend.virgl` |
| box64 失效通路 | 只有 core（有效面）；rint/符号桥「删了会坏」从未证明；HTTPS/媒体 3 符号无用例触达 | rint A/B（revert→重建→core 期望红）；HTTPS/gstreamer 起播探针；构建期扫 guest UND vs wrap 表差集门禁 |
| 构建链 | CI 有毁灭性演练（干净容器全量+sha 复算）但本地不可复跑；增量/幂等/patch 漂移零覆盖 | `scripts/verify-build.sh`（待建）三档：from-scratch 产物断言清单 / incremental 触发重编验证（touch glib 源必须重编）/ patches 漂移断言；每条断言配「人为损坏必须红」 |
| 生命周期 | virgl_child 关停、attach/detach 循环、反复停启无用例 | 停启 20 轮探针（task 数稳定）；attach/detach N 次 + Faultlogger=0 |
| 音频宿主消费 | `underrunCount` 写死 -1，「宿主真在出声」从未度量 | [AudioBroker] close 统计进归档参与判定 |

## 新探针/判定器建设清单（按性价比排序）

1. **零设备成本**：host_tests 扩容（ToplevelManager/PopupManager/MoveGrab/FirstVisibleModalLocked 打桩注入）；region 空非空、frame callback 单调性单测；`make test` 门禁
2. **低成本（现有通道）**：EnumDisplaySettings/GetVersionEx/盘符枚举/getenv 探针走 result-json；border 指标拉进 coverage.py required；graphics-smoke 结果 JSON 增 transport 字段；WGL 扩展断言进 result metrics
3. **中成本（新载荷）**：`winehua_win_smoke`（owner+三件套/固定尺寸对话框/模态框/Shell_TrayWnd 各一，一次覆盖 6 个窗口管理点）；graphics-smoke `--fullscreen`/`--damage-partial`/`--resize-mid` 三变体；Vulkan 非对称四象限载荷（rgba-quadrants 镜像即 FAIL，直接吃掉 flipY/letterbox 两类回归）；六类缺能力兜底各一条最小用例（dualSrcBlend src1 混合/A2C 透明叶片/divisor=2 实例/SNORM RT 负值/QUERY_EVENT/sRGB backbuffer）
4. **中高成本（新通道）**：宿主协议摘要落盘 + `protocol-log` 判定器；input-smoke 探针 + uitest 注入；`backend.virgl` 进 job 协议；HTTPS/gstreamer 起播探针
5. **高成本（专项）**：手柄子系统（文档+契约检查+注入探针）；BOX32 长跑耗尽；mallochook A/B 收口

## 各模块明细

### wine 平台机制 + 窗口/输入（~30 点）

| 定制点 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| broker 拉起 wineserver + socket 就绪 | 必要：沙箱 NCP 无 execve/fork | OK，已收口单点 | 全套件隐式 | 有效=core PASS；补宿主锚点三态进归档断言 |
| noexec 三连 + PE 头页 pread + 假成功换匿名页 | 必要：noexec+页权限创建时定 | OK 机制级 | core 隐式 | 失效=guest cppcrash；缺口：加壳程序（AV 类）无探针 |
| 盘符映射 ohos_file.c | 必要：沙箱无 symlink | 更优：mountmgr 双源收口 | 无 | 新探针：GetLogicalDrives+Z: 读写进 result |
| dnsapi libresolv_musl | 必要：musl 无 libresolv | OK | **已覆盖**：core dns-api 用例 | 有效=解析 IP；失效=resolve_error |
| freetype 扫描 /system/fonts | 必要：无 fontconfig | OK | 无 | 固定帧含中文+visual validator 或字体族名上报 |
| LC_ALL/locale | 必要：musl locale 缺位（配 po 编译两步缺一不可） | OK | 无 | 低成本版：result 上报 GetACP/LOCALE 名 |
| schannel 禁 CHACHA20 | **未实锤**（D 类） | 更优：改握手失败才降级的能力探测 | 无 | https 请求探针 + 开/关对照 |
| nulldrv_CreateWindow 特判 | 条件必要（回退时服务进程须能建隐藏窗），无解释须补 | 改动合理，补注释即可 | core 隐式 | 失效=服务进程建窗崩溃 |
| WINEHUA_WINDOWS_VERSION | 条件必要，默认关符合原则 23 | OK（CrossOver 成熟方案） | 无 | env 注入+GetVersionEx 进 result |
| setupapi needmedia 上限 / shell32 CLSID 容错 | 条件必要：防挂死/不炸 explorer | OK | 无 | 低优先；假 media/假 CLSID 目录探针 |
| wineboot 跳 legacy profile thunk | 必要：OHOS prefix 无旧 profile | OK | gate clean env | 有效=clean core PASS |
| vkd3d/dxvk 私有 swapchain 全链 | 必要：宿主无任何 Vulkan 原生 WSI | OK（备选不可行） | wine-vulkan-present/d3d12/dxvk | 有效=presentFrames>0；失效=swapchain 创建失败 |
| TRANSFER_DST | 条件必要，全局生效伤及非 vkd3d | 仅 vkd3d 档加 | dxvk-500k-routes（已知红）+d3d12 | 有效=d3d12 gears 出图；加 dxvk 档去 DST 对照跑 |
| loader overlay + 档位字面特判 | 必要：overlay 是 Unix 路径非 C: 前缀 | 更优：宿主下发展开值/旗标 | dxvk（module_is_native）+gpu-diagnostics | 有效=断言加载 dxvk 路径；失效=回退 builtin 链 |
| app_id class 后缀 | 必要：免宿主几何启发式 | OK，可扩展 | 无 | 见 protocol-log 探针 |
| 桌面模式 geometry/win_data/ensure_contents | 必要：虚拟桌面全窗口须 subsurface 根；修 Vulkan 首帧死锁 | OK | core 隐式 | 失效=桌面黑窗/present 挂死 |
| min/max 约束+可调整性判据 | 必要：608×190 最小尺寸撑大 1×1 辅助窗（实锤） | 更优：改读 WM_GETMINMAXINFO | 无 | 探针：固定尺寸对话框，断言宿主收 min==max |
| is_window_managed 三件套 | 必要：wayland 无工具窗口概念（企业微信实锤） | OK；边界：判据最前压过 WS_EX_APPWINDOW，可挪进 popup 分支缩小影响面 | 无 | protocol-log 探针（断言 toplevel 数） |
| 标题栏正方形+1px 圈+删字体行 | 必要：双度量 34×46（上游缺陷）/扁平化回归/字体绕过 locale | OK；defwnd.c 是最大冲突敏感区 | 无 | 标题栏区域像素断言；构建期断言 aero.res UTF-16 |
| 最小化豁免 present_rect | 必要：-32000 哨兵被覆盖→还原握手死循环 | OK | 无（无独占全屏用例） | 探针：最小化/还原序列，断言帧计数恢复 |
| modal 私有协议 | 必要：xdg 无法表达模态禁用 | OK（协议优于启发式） | 无 | protocol-log 探针 |
| 相对指针+enter 静默校准 | 必要：游戏原始输入；差分污染实锤 | 更优：删 env 改代码默认 | 无 | dinput_probe --automation：enter 后首帧 delta=0 |
| SIMULATE_RESOLUTION/1280x800 | SIMULATE 条件必要（默认关）；1280x800 必要 | 更优：查清 vd=0 根因收口 mode 表 | 无 | EnumDisplaySettings 模式表进 result（成本最低） |
| 手柄 bus_ohos | 必要：沙箱无 evdev 生态 | OK（默认关=原则 23 样板） | 无 | gamepad socket 注入→DirectInput 枚举进 result |
| winehua_keep | 必要：wineserver 空闲超时杀桌面 | OK | core 隐式 | 失效=套件启动即结束 |

### wine 图形链（13 点）

| 定制点 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| readback 呈现 | 必要：wl_shm 是唯一通用通道 | OK | opengl（但不钉后端，回落 shm 时此路没跑） | 有效=PASS+readbacks>0；失效=帧错位 |
| 缓冲复用 3 slot | 必要：逐帧分配是主要 CPU 开销 | OK | 无独立 | host 加 fps 地板判定（metrics 已有 frames） |
| BGRA 直读 | 必要：省 swizzle | OK | opengl（象限颜色即判） | 已被 rgba-quadrants 覆盖 |
| 零拷贝页 | 必要：跨 .so 传 surface_id 只能走共享内存 | 更优：契约字面量抽共享头 | 无（计数只打 stderr） | 有效=zero_copy>0 且 readbacks=0；失效=静默降级需新计数判定 |
| pbuffer 重建 | 必要：三环失配→画面下移 | OK | 无中途变档用例 | 探针 `--resize-mid` 两帧都过象限；失效=第二帧偏移 |
| 扩展索引修复 | 必要：真上游 bug | OK，可上游化 | 无 | 启动断言 GL_EXTENSIONS 含 VBO，进 result |
| guest EGL 预载 | 必要：OHOS 无系统 libEGL | 更优：去版本字面量改扫描/manifest | opengl 间接 | 断言 EGL_LIBRARY_PATH 非空+预载失败计数=0 |
| 私有 swapchain+TRANSFER_DST | 同上 | 见上表 | d3d12/wine-vulkan-present | 同上 |
| mixed overlay 三笔 | 必要 | 更优：旗标化 | dxvk/d3d12/gpu-diagnostics | gpu-diagnostics 加路径 coverage 断言（不只报告） |

### 宿主 compositor（20 机制）

| 机制 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| 两段式合成管线 | 必要：持锁 blit 25ms/帧实测 | OK | core 间接 | `WINEHUA_FRAME_TRACE=1`：commit avg<3ms 有效，>20ms 失效 |
| SHM 全屏直传 | 必要：40-85ms→0 | OK | 无全屏用例 | `--fullscreen` 变体：直传标记+四象限铺满 |
| 局部重绘 | 必要：25ms 整帧的增量出路 | OK | 无 | `--damage-partial` 变体：R 面积<全屏 |
| 层序单一来源 | 必要：双序即经典 bug | OK | host_tests zorder_test | 端到端：两窗交叠 raise，截帧判上层色占比 |
| fsPriority 仲裁 | 条件必要（根因在 wine 矩形判据） | 更优：发送前判 foreground | 无 | 探针：记事本+全屏游戏，点击落游戏 |
| FitRect+PresentedFrame 契约 | 必要：红警2 二次缩放事故 | OK | host_tests geometry_test | 全屏+点击四象限命中校验 |
| ZC 零拷贝层 | 必要：GPU 游戏唯一通路 | OK | wine-vulkan-present/d3d12 | 遮挡重绘：ZC 游戏+toast，判 toast 区被恢复 |
| SHM/blit 纯函数 | 必要 | OK | host_tests 3 个 | 门禁=`make test` |
| DisplayPolicy 三路由 | 必要：协议字段判据零启发式 | OK | host_tests display_policy_test | PC 菜单弹出失效=客户区被开成子窗 |
| commit 双缓冲 | 必要：协议语义 | OK（死字段可删） | 无 | [MW-COMMIT] w/h/geo 断言 |
| wl_region+set_input_region | 条件必要；直写违反双缓冲 | 更优：真 subtract+pending 化 | 无 | host 单测；失效=subtract 后仍非空 |
| frame callback 墙钟 | 可删（改 CLOCK_MONOTONIC 零风险） | — | 无 | 单调性单测 |
| ToplevelState 权威 | 必要：消掉手工清状态类失步 | OK；猜测可由 SC_RESTORE 握手替代 | 无 | TryAutoRestore 打桩 host 测试 |
| xdg 状态机+max_size 启发式 | 必要；启发式前提已被现行 wine 推翻 | 先埋日志实测命中率，零命中即删 | 无 | ShowWindow(SW_MAXIMIZE)：应走 set_maximized 路径 |
| 桌面 root 识别 | 必要：ghost 同尺寸，title 唯一判据 | OK | 无（人工） | 冒烟：截帧判背景+任务栏 |
| 会话生命周期建模 | 必要：热重启双循环抢帧实锤 | OK | 无 | 二启冒烟：桌面出图+首击响应 |
| modal 模态组 | 必要：owned 语义 | OK | 无 | host 单测 FirstVisibleModalLocked；端到端点击 owner 应被阻断 |
| popup 承载 | 必要：子表面越界必须独立承载 | OK；两处 P2 已标 | 无 | notepad 右键菜单截帧+点击生效 |
| move_grab 绝对定位 | 必要：相对往返位移发散实测 | OK | 无 | 拖 D px 位移==D；失效=>D 发散 |

### 宿主 input（14 机制）

| 机制 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| 编排门面+四层拆分 | 必要：wl 线程约束 | OK | 无 | 探针收 WM_LBUTTON*；失效=[WL_NAPI] 有而 [Input] 无 |
| 坐标变换+目标裁决 | 必要：letterbox 逆映射+菜单独立 surface | OK | 无 | uiInput 点菜单越界区，探针坐标落菜单内 |
| InputQueue 去重 | 必要：线程边界+125Hz 洪泛 | OK | 无 | swipe 60s→[Input-DROP] 出现且无 invalid object |
| enter/leave 三语义 | 必要：桌面模式 surface 级 | OK（红线禁统一） | 无 | 点两窗成对 PTR PRESS+KBD ENTER |
| 修饰键快照+会话复位 | 必要：不清则 Ctrl 跨会话卡键 | OK | 无 | Ctrl+A 后热重启，GetKeyState(VK_CONTROL)==0 |
| 脉冲拉伸 STRETCH | 条件必要（仅触屏残留） | 更优：治本在 ArkTS 手势层 | 无 | 快速 tap 断言 down_up_gap∈[90,150]ms；物理鼠标路径出现 STRETCH=bug 信号 |
| keymap blob | 机制必要；再生链缺失=D | 更优：构建期从可读源生成 | 无 | ToUnicodeEx 全表比对 ArkTS 期望 |
| 相对指针 | 必要：FPS 无限位移 | OK（已收敛；±512 无测量依据） | 无 | ShowCursor+ClipCursor→rel_delta_sum>0 且 GetCursorPos 不变 |
| 约束 lock/confine | 必要 | OK（serial 宽容是注明取舍） | 无 | 挂 lock→LOCK pointer 日志；失效=重试风暴 |
| warp 同步 grab | 必要 | OK | 无 | SetCursorPos 后拖动无放大位移 |
| 光标冻结/隐藏门禁 | 必要：误冻 6.5min 实锤 | OK（IPC 出锁外已修） | 无 | 相对模式→LOCKED；杀窗口→UNLOCKED |
| wl_seat 资源池 | 必要 | OK | 无 | SetCursor 自绘无协议错误 |
| text_input IME | 必要：中文唯一通道 | 更优：修锁外读+删死代码 | 无 | inputText 中文→WM_CHAR 序列==期望 |
| 手柄 controller/ | 条件必要 | 缺文档+协议双端手工同步 | 无 | socket 注入→joyGetPosEx 变化；keyboard_legacy 降级门禁 |

### 宿主 graphics（18 机制）

| 机制 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| 共享 EGLDisplay | 必要：驱动竞争 SIGSEGV 实测 | OK | 全套件间接 | 失效=[EGL] eglInitialize FAILED |
| VSync 节拍 | 必要：防双等待 | OK | graphics-smoke displayFps | gears 600 帧+displayFps>0；失效=-1 |
| 无帧跳过+失配复绘 | 必要：1683 中间态黑边实测 | OK | 无 resize 载荷 | 转屏探针：固定帧不拉伸；失效=黑边 |
| fit/letterbox+输入锚 | 必要：红警2 点不动根因 | OK | 间接 | 点击象限变色载荷 |
| ZC attach 轮询 | 必要：Maleoon 无 dma-buf | 更优：注册表事件推送 | 无（x86 走 softpipe） | 结果 JSON transport 字段断言 |
| ZC 8 次降级 | 必要兜底 | 更优：改时间窗（次数随帧率漂移） | 无 | env 挂 update→断言 CPU_FALLBACK 且 shm 帧 PASS |
| venus flipY | 条件必要（D→机制已明） | 更优：朝向进 surface 元数据 | 无 | 非对称四象限载荷（镜像即 FAIL） |
| ZC 遮挡重绘 | 必要：GL overlay 不进 CPU 层序 | OK | 无 | 全屏游戏+弹菜单判遮挡区像素 |
| broker 后端判定 | 必要：回 shm 留因 | 基本 OK（BOX64_EMULATED_LIBS 双源收口） | gpu-diagnostics（只报不判） | active/note 进 result JSON |
| 三路启动+4s 等待 | NCP+in-process 必要；waitpid 分支可删 | 更优：4s 等待异步化 | wine-vulkan-present | 失效=virgl socket wait FAILED |
| .ready marker | 条件必要（仅 NCP 跨进程需要） | 更优：并入 query reply 可删文件 | 无 | attach 后 2s 无 consumer attached=失效 |
| virgl_child IPC+关停 | 白名单必要；关停方式不合理 | 更优：requestStop 自然返回+2s 看门狗 | 无 | 停止后断言 vtest exited rc= 日志（现 _exit 必缺失） |
| perf forwarder | 条件必要（仅诊断档） | OK（200/250ms 可合一） | 无 | 诊断档跑 d3d12 断言 [VIRGL-PERF] 行 |
| shadowTrace→env 映射 | 必要：诊断唯一入口+防注入 | OK 单一映射表 | gpu-diagnostics env 注入面 | 档位断言 [VENUS-FRAME-TIMELINE] serial 连续 |
| GL blit presenter | 必要：fence 保序跨 context | 基本 OK（每帧 2 次 makeCurrent 是代价） | 无（virgl 档零套件） | arm64 virgl 档条目；失效=blit dropped 增长 |
| venus Vulkan presenter | 必要：UNKNOWN→可重建有实测依据 | OK | wine-vulkan-present | presentFrames≥N；失效=swapchain 重建风暴 |
| Manager 2500ms attach 等待 | 必要（真实竞态） | 不合理：阻塞 present 线程；更优=立即返回 1+16ms 重试 | 未覆盖竞态 | attach 延迟 env 注入→无 timeout 日志且帧数达标 |
| NativeWindowLease 双释放 | 必要：teardown abort 实证 | OK | wine-vulkan-present | attach/detach 循环 N 次 Faultlogger=0 |

### 宿主其余（wine/proc/bridge/audio，~25 点）

| 机制 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| broker 单通道 spawn | 必要：NCP 不能嵌套 | OK（真 connect 判就绪） | core 间接 | 三态用例：kill→failed / 主动退→stopped / stopAll→drain |
| NCP 路由 | 条件必要（手机形态） | 不完整：kill/判活/等退出三份分流 | 无 | NCP 后端 /proc 必 ENOENT，只认 ncp-exit |
| phone fork 实现 | 条件必要 | OK | 无（缺手机设备） | 握手成功+子进程被 reap |
| phone virgl relay | 条件必要 | 缺 magic/版本，三处同步 | 无 | 加版本后双路径回包对拍 |
| wineserver 截获 | 必要：纯 Unix ELF 不走 PE loader | OK | 无专项 | ws step7+socket 就绪；失效=精简基线缺 BOX64_* |
| wineserver 锚点三态 | 必要：NCP 无 /proc，退出回调唯一权威 | OK（热重启保锚点已实测） | gate 只盖 prefix 复用 | 三触发各断言一条 |
| prefix 就绪判定 | 必要：防过早 spawn | OK（status 文件字面契约双源） | gate clean/reuse | 有效=wineboot completed+state:ready |
| env 基线+__env 通道 | 必要：NCP 不继承 environ | 半途：EnvSpec 只有 SpawnViaBroker 在用 | gpu-diagnostics 间接 | env 探针用例：exe 打印 getenv 进 metrics |
| AppendStableDxvkEnv | 必要：explorer 子孙同源 overlay | 别名传参靠注释纪律+反查表双份 | 无 | extraEnv 启动判该键唯一且未被覆盖 |
| SetHostShadowProfile | 条件必要：子进程 setenv 不进去 | 40 处 strcmp 应改数据表 | 无 | [NAPI] host shadow profile= 行与档位一致 |
| WINEDEBUG 选择+per-exe 特判 | 默认 -all 必要；特判可删 | 死通道：先换位修复再删特判 | smoke.py 已拦声明（负向防护） | 修序后 trace:module 断言，随后删特判 |
| 音频 memfd ring+两级控制面 | 必要：arm64 不能 dlopen 音频 .so | OK | audio 套件 | 补宿主消费度量（underrun 现写死 -1） |
| 来电 RESUME/capture 惰性 | 必要：不显式 Start 永久无声 | OK | 无 | 宿主直接 pause 触发 interrupt，断言 resumed |
| audio 协议头双份 | 必要：独立构建链 | 双源无同步检查 | 无 | include 单头+check 脚本 diff 断言 |

### ArkTS 层（~18 点）

| 项 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| 机型白名单 | 条件必要但静默 | 迁资产表+命中打点 | 无 | dxvk26-requirements 全 PASS 才许默认 modern |
| perfProfile 写死 | 条件必要（vkd3d 无 flush 发布点） | 假装读持久化但无写入方 | d3d12（正确档） | 改档双跑 d3d12+gears；失效=computeUav 蔓延 |
| [[SMOKE]] 钩子+ets/smoke 进产品 | 条件必要但违反原则 22 | 摘法已写明（前置=AA 通道） | gate+core 依赖它 | 摘后 gate 全绿+grep SMOKE 零命中 |
| 两套持久化并存 | 必要落盘；冷启不恢复是 D | 不合理：双重回退源 | 无 | 选 modern→冷启→断言档位保持 |
| 手势状态机四份 | 状态机必要，复制不必要 | 收口 WinePointerStateMachine 单类 | 无 | 同一注入脚本跑四处基线对照后收口 |
| desktopLauncherVisible | 可删（整链） | — | 无 | 删后旋转/改分辨率断言 notifyToplevelResize |
| decor 校准残留 | 可删 | 保留 cornerRadius+title | 无 | resize 后 [MW-RESIZE] 尺寸==round(w×s) |
| 墓碑 Set | 必要但须挂会话清理 | 清理挂 state:stopped | 无 | 热重启后新窗口不被 isDestroyed 拦 |
| move_end 定时器 | 2000ms 功能保留，3000ms 诊断删 | 拆开 | 无 | drag 后 popup 跟随坐标断言 |
| applyModePolicy 分叉 | 必要：Pad 无自由窗口 | OK | 无 | --ps fusionMode 断言 subWinId shown |
| FusionWindowManager | 必要：WMS 不抬 popup 实锤 | OK（正解 subsurface 合帧在做） | 无 | 点非焦点子窗→raiseToAppTop+popup 顺序 |
| ModalWindowManager | 必要 | OK（居中特判有实测） | 无 | --ps 对话框程序：坐标在屏内+owner 点击被阻断 |
| PopupWindowManager | 条件必要 | fullscreenPopup 疑死分支待实测 | 无 | 右键菜单判非 fsPopup+点击生效 |
| WineEnvService 状态机 | 必要 | 基本合理（轮询可事件化低优先） | core 间接 | autoStart→state: starting→ready 文案断言 |
| IME 桥+AA 通道 | 必要 | OK | 无 | inputText→[insertText]+commit 到达 |
| InputSettingsService 手柄段 | master 可删（组件不存在） | 按分支拆 | 无 | 删后只验触控板 |
| Box64Dynarec 档位表 | 必要 | OK（样板形态） | 无 | 四档各跑一个 smoke exe 断言 env 落盘 |

### virglrenderer（~20 点）

| 定制点 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| EGL 无 GBM 初始化 | 必要：OHOS 无 GBM/libdrm | OK | 启动隐式 | host log EGL init 成功；失败=virgl_egl_init -1 |
| vtest 封 dlopen 库+present 桥 | 必要：无 WSI+无 execve | OK（命令号已文档化） | wine-vulkan-present/d3d12 | present serial 连续；失效=queue-busy 连续/白屏 |
| present pacing | 条件必要 | OK | 无消费 | PERF_SUMMARY 间隔≈deadline |
| shadow 内存桥 | 必要：Maleoon 无 dma-buf 导出，唯一路径 | OK（不变式已写明） | dxvk/d3d12 隐式 | SHADOW_TRACE=1 后 shadowFnv==hostFnv；失效=d3d12-1000f 停滞 |
| flush/invalidate 接管 | 必要：上游两 dispatch NULL | OK；隐患：只认字面 `precise`，产品档 `precise-dirty` 走 legacy 分支（三层字符串契约） | 无 | Gate C 档 mapping-sample equal=1 |
| invalidate 保留 pending | 必要 | OK | 无 | 动态 UBO 用例无半旧帧 |
| Maleoon GPU upload quirk | 条件必要（实测） | 更优：启动时可见性探针替代 vendorID | 无 A/B | GPU_UPLOAD=0 对照 dxvk-dynamic 出错→quirk 确必要 |
| coverage 排序 | 条件必要（性能） | OK | 无 | PERF_SUMMARY 六相位耗时 |
| modern 同步分支 | 必要 | OK | dxvk-modern-baseline | 失效=QueueSubmit2 generation 竞争 |
| per-client fence | 必要：上游全局 TODO 实锤 | OK | 无多窗口用例 | 双窗口同开无死等 |
| busy-wait+glFinish | 条件必要（EGL fence 缺陷止血） | 不合理：每帧付 glFinish；更优 per-submit fence | 无 | SYNC_GL_FINISH=0 对照帧率且无死锁 |
| VIRGL_DISABLE_EGL_FENCE | 条件必要 | OK（显式开启） | 无 | egl-main 档 present PASS |
| GetFenceStatus OOM 重试 | 必要：Maleoon 瞬态 OOM 毒化 ring | OK（DEVICE_LOST 透传） | dxvk-long/modern-long | 1h 无 ring fatal |
| Z32 仿真 | 必要（上游也是 fake） | 两处弱点：无 FBO 探测、ES3 有 D32F 白丢精度+type 布局未验证 | 无 | depth-readback 用例量化步长；失效=FBO incomplete/条带 |
| sRGB destination | 条件必要 | 隐患：判定与实现脱节（GBM 门内） | 无 | sRGB 附件固定帧与 UNORM 对照无偏色 |
| x86_64 EGL 特判 | 条件必要（express GPU 崩实测） | 更优：运行时回退 | 无 | 模拟器无 real share context is NULL |
| fence 指针缓存 | 必要：libepoxy 延迟 wrapper abort | OK（已真机验证） | 无自动 | 手机停/重启进程存活；失效=cppcrash |
| vtest wake pipe | 必要 | OK（主仓 _exit 是 B 类） | 无 | 停启 20 轮 task 数稳定 |
| fd 所有权 thread 模式 | 必要：沙箱拒 sealed fstat | OK | 无 | RESOURCE_TRACE=1 无 EBADF |
| bool spec 冻结 | 条件必要（实测） | OK（默认关） | 无 | 开/关对照 frozen 计数 |

### dxvk legacy + modern（~25 点）

legacy：flush/invalidate 协议（必要，负向验证=关 env 读回矩阵必红）、flush 批处理（条件必要，_STATS 断言 calls≪ranges）、RELAXED_FEATURES（必要但文档失真，更优=设备自动判定删 env）、BC 解压（必要，bcSamplingFunctional 已 required）、dualSrcBlend（必要，**无用例**，新增 src1 混合）、A2C opKill（条件必要，取舍有论证，**无用例**，透明叶片用例+关 quirk 负向）、instance divisor（必要，**无用例**，divisor=2 用例）、SNORM RT（必要，**无用例**，负值保留用例）、combined sampler（休眠兜底，910 未验证是存留依据）、bool 冻结（必要，负向 A=B 一次锁死）、CubeArray Dref（必要，heaven 矩阵已覆盖）、border color（必要，**指标产出但未进 required**——拉进 coverage.py）、event query（必要，**无用例**，QUERY_EVENT 用例）、present sRGB（条件必要，**夹带提交+无用例**，sRGB backbuffer 用例）、Heaven quirk（**可删**）。

modern：flush 协议（优于 legacy，双档对照现成）、批处理（**产品休眠**，应随 isWineHuaVenus 自动开）、dualSrcBlend（比 legacy 严谨）、BC 回退（bcdec 相对 include 是布局地雷）、border/event（同 legacy 缺用例）、能力策略（优于 env 放行；geometryShader 等仍硬请求，910 建不起来由白名单兜）、swapchain factory（位置正确回退序，vkd3d 升 2.8+ 可删，建议加一次性命中日志）、未移植清单（920 上逐项行为探针，「未验证」变「已验证」）。

### box64（15 笔）

| 定制点 | 必要性 | 合理性 | 现有覆盖 | 建议验证 |
|---|---|---|---|---|
| InternalMmap 收敛 | 必要：RWX EPERM+noexec | OK | core | /proc/self/maps 见 PROT_EXEC 匿名段 |
| mmap fallback 链 | 必要：aarch64 无 NOREPLACE | OK | core | 失效=低 4GB 申请失败日志 |
| LIBBOX64_SO | 必要：无 execve | OK | core | 失效=wine_child 秒退 |
| prctl SIGSEGV | 必要：残留地址被内核解引用 | OK（仅 .so 模式成立） | core（wineboot/rundll32） | 失效=Faultlogger prctl SIGSEGV |
| IS_FILE 不查 exec | 必要 | OK | core | 失效=启动期拒载 |
| 跳过必败 RWX | 条件必要（perf） | OK | core 间接 | core 平价即可 |
| musl 20 patch | 必要：guest UND 只能由 wrap 表满足 | OK | 仅启动路径 | 失效=BOX64_LOG=1 grep `Symbol not found` |
| mallochook passthrough | 条件必要但自述矛盾 | A/B 收口：worktree 换回上游跑 core，挂=实锤保留 | core | 长跑量 RSS |
| threads.c 裸调用 | 当前不可达（152 个 .so 零导入实测） | 4 行 NULL 守卫 | 无 | 静态 grep+动态 BOX64_LOG=2 命中 0 |
| BOX32+bump 堆 | 条件必要 | 更优：free 归还整段或复用 customMem32；256MB 非 LIFO 是长跑风险 | x86 变体 | 分配-释放循环 30min；失效=`OHOS box32: bad free` |
| musl_box32_stubs pc 目标 | 条件必要 | OK（ucontext stub 有限制） | 无（pc 无 CI） | 链接零未定义符号断言 |
| rint wrap | **必要（D→A 实锤）**：libgallium 仅从 libc.so 导入 rint，wrap 表缺即 eager 失败；上游 wrappedlibm 本有 GOWM(rint) | OK；nearbyint 不同步是对的（guest 未导入，别顺手开） | opengl-x64/x86 | A/B=revert 重建跑 core 期望 `Symbol rint not found` |
| 7 符号桥接 | 必要：6/7 有实导入方 | OK 但被动（崩一个补一个）；更优=构建期 UND-vs-wrap 差集门禁（方法已验证：489 UND libc 类零缺口） | dns-probe 只护 resolver 4 符号 | HTTPS/gstreamer 起播探针 |
| 四笔清理 | 可删 | OK | — | 编译+core 平价 |

### 构建链

机制层（stamp/断言/manifest/架构隔离/下载幂等）全部必要保留；CI 已是毁灭性演练但本地不可复跑、只测 arm64、增量零覆盖。改方案项：deps `-newer` 清单补 glib/gstreamer/pcre2（现在改源码不触发重编）、soname 清单改 manifest 单源+glob 硬失败、FFmpeg 静默降级改硬失败（BUILD_FFMPEG=0 显式豁免）、字体 sed 挪回 wine.inf 定制提交、wayland-scanner 改 host-tools（Darwin 分支已有实现）、build.sh 第二入口删除、发布四步改 `package.sh release`+trap、代理读 `.ohos/proxy`。验证建设：`scripts/verify-build.sh`（待建）三档（from-scratch 产物断言/incremental 触发重编/patches 漂移），CI 内联 Python 落成 `scripts/verify_hap_payload.py`（待建）两端复用；每条断言配「人为损坏必须红」演练。
