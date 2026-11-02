# PR #78 完工清单

这份清单用于把兼容性汇总从 Draft 推到可审查、可合并状态。历史设备记录不等于当前候选版本的完整验收。

## 当前门槛

| 门槛 | 状态 | 完成条件 |
|---|---|---|
| 分支同步 | 集成候选已同步 | `integration/pr78-upstream` 已合入最新 `master` / `a001e6d` 并解决冲突；PR #78 头仍待候选完整验收后替换 |
| 变更分层 | 已拆分候选 | 技术分支已移除独立游戏库产品文件；`integration/pr78-main-ui` 通过 merge 继承公共修复，产品配套 [PR #79](https://github.com/winehua/WineHua/pull/79) 为 Draft |
| 主机回归 | 分层通过 | 技术版 7 组 JS、9 个 renderer 用例及新增 EGL/loader/特性/覆盖/打包回归通过；产品版 9 组 JS 通过；原 Linux 27 阶段属于旧基线证据 |
| 完整构建 | 完整 Guest/Native/HAP 重建通过 | 隔离 Linux 构建完成 Guest 依赖、Wine 与 canonical assemble；重复组装哈希一致，导入 4,591 个输出并通过架构/哈希核验 |
| 设备验收 | 主套件大部分通过，Wine Vulkan 聚合仍阻塞 | core 两轮 4/4、DXVK 3/3、D3D12 3/3、D3D9 4/4、X11 GL/DXVK 各 1/1；Wine Vulkan 前两轮 4/4，第三轮及后续诊断仍会在同一 Host 会话的短生命周期客户端切换中间歇保留 `0xdeadbeef`；单项 x86 新会话 5/5 |
| PR 状态 | Draft | #78 仍为旧基线 Draft 且与 master 冲突；#79 为产品候选 Draft。补齐重建、设备验收与 CI/review 后再改 Ready |

## 集成技术候选实机证据

当前设备包为 `build/integration-hap-v7-technical/desktop-candidate-signed.hap`，HAP SHA-256 为 `dfeef6ad69321010abeed8b77edd1bb73c0f217b83a3d8a1bfe478e5bff4e223`；运行时载荷 SHA-256 为 `0aa1eac9b72376ea5094441b5d5b27067b1266dcd275c442d5148e0960404e0c`。该包已移除独立游戏库，保留共享输入/图形修复。以下均显式使用 Wayland 路线和 `prefix=reuse`，不能替代 X11 默认路线或全新运行时验收。

| 套件 | 主机判定 | 集成工作树中的归档 |
|---|---|---|
| core（当前源码编译的两架构探针） | PASS 4/4 | `build/automation-logs/core-technical-core-dfeef6/` |
| wine-vulkan | PASS 4/4 | `build/automation-logs/wine-vulkan-technical-wine-vulkan-dfeef6/` |
| dxvk | PASS 3/3 | `build/automation-logs/dxvk-technical-dxvk-dfeef6/` |
| d3d12 | PASS 3/3；1000 帧、gears/triangle 各 600 帧 | `build/automation-logs/d3d12-technical-d3d12-dfeef6/` |

独立 D3D9 套件当前为 2/4：x64 D3D9 和同档位 D3D11 完成呈现及截图，x86 两项仍因 DLL 加载失败而退出。Wine overlay 搜索补丁已通过主机 WoW64/档位隔离测试，待新 Wine 构建后实机确认。该套件现要求专用档位下两架构、两 API 均成功呈现；普通 DXVK 的完整 D3D11 功能矩阵判据保留。

系统崩溃栈已确认并修复 EGL 探针错误载荷导致的手机首启 SIGSEGV，设备标记为 `pass:payload=OHNativeWindowBuffer`。运行库迁移均先核验旧文件、备份、停引擎、逐文件回读并最后更新标记；未卸载、重置 prefix 或清除游戏/存档。

## 本地验证

- TypeScript/ArkTS：桌面输出、指针、游戏库、触控、鼠标模式和虚拟按键回归通过。
- C++ host：20 个纯逻辑用例通过；事件总线测试已对齐当前 21 个事件，z-order 测试改为字符串内容比较。
- 字体策略：`wine.inf` 以 UTF-8（含 BOM 兼容）读取，中文字体别名回归通过。
- HAP 构建：干净 `hvigor clean` 后 `assembleHap` 成功；候选包 CRC、runtime manifest/hash 和 native markers 校验通过。
- 设备启动：首次候选包因 native 依赖未被 HAP 收集而触发 `libentry.so` 加载失败；改用实体 `entry/libs/arm64-v8a` 并干净重打包后，28 个 native 库进入 HAP，设备启动保持运行。

## 设备 smoke 状态

候选 HAP 已安装到设备并保持常亮。最初 core 无 summary 是手工生成的测试 JSON 含字面量换行转义导致，后续已由合法的 suites/manifest 替换。修复 SmokeRunner 错误日志后定位到此问题。

OpenGL 随后失败于 `no compositor display sequence observed`，原因是 smoke Want 将管理页拉回前台，而手机普通会话有意不自动打开桌面。最终修复将前台准备放入 SmokeRunner 的共同执行路径：冷启动、二次 Want、开发面板均传入当前调用方的 UIAbilityContext，等待 root、请求 DesktopAbility，并确认 surface 就绪后再启动测试。普通无 smoke 请求的启动不触发前台切换。`prefix=reuse` 在 runtime 升级待处理时会失败并保留环境，只有显式 clean 请求可走测试重置。

### 旧基线候选归档

| 套件 | 主机判定 | 本地目录（相对候选工作树） |
|---|---|---|
| core | PASS 4/4（含两架构四象限截图） | `build/automation-logs/core-core-foreground-fixed/` |
| core（Linux 新编 DNS） | PASS 4/4 | `build/automation-logs/core-core-linux-dns-rebuilt/` |
| wine-vulkan | PASS 4/4 | `build/automation-logs/wine-vulkan-wine-vulkan-pr78-followup/` |
| dxvk | PASS 3/3（含 cube 截图） | `build/automation-logs/dxvk-dxvk-pr78-6bf3abe/` |
| d3d12 | PASS 3/3；1000 帧 + gears/triangle 各 600 帧 | `build/automation-logs/d3d12-d3d12-pr78-6bf3abe/` |
| DXVK 专用 D3D9 档位 | FAIL 0/4；载荷缺少 x64/x86 DXVK `d3d9.dll` | `build/automation-logs/dxvk-d3d9-dxvk-d3d9-explicit/` |
| DXVK 专用 D3D9 档位（重建 DLL 后） | FAIL 1/4；x64 D3D11 通过，x64 D3D9 建设备失败，x86 DLL 加载失败 | `build/automation-logs/dxvk-d3d9-dxvk-d3d9-rebuilt-138733/` |

后三套件对应 `6bf3abe` 候选；第一次完整 core 通过时已包含同样的共同前台逻辑，但尚未加入 reuse 升级保护。HAP 的 Native/ArkTS 是本轮构建；Wine/Mesa/VirGL 和最初 smoke EXE 复用已有产物。这些不是完整运行时从零重建证据。Linux 另已重新编译两架构 DNS probe，图形探针仍来自已有 Wine PE 构建，构建来源记录在单独 core payload 的 `build-provenance.json`。

`6bf3abe` HAP SHA-256：`98227b4b76a080594604576ef701fba09f9e06c082c3609585129cea7d741746`。Wine payload SHA-256：`d6eef4ffaecf84e45fc431d878aeaf108471b9fddb2df061b9e6fa0e4c1cf58a`。

当前候选仍需要与最新主线完成集成并完整重建。已有旧基线通过记录不能代替集成后的验收，也不能据此标记整个 PR 完工。

2026-11-02 已完成集成分支的完整 Guest/Native/HAP 重建和重复组装核验；当前不再缺构建证据。剩余技术阻塞是 Wine Vulkan 聚合套件的快速多客户端切换：shader、descriptor、dispatch 和 Host 提交均到达，但某个新客户端的首个 compute 偶发不写回。多种等待、barrier、读回和上传策略均已证伪，失败记录保留在 `build/automation-logs/`。产品路径已取消强制实验性 inline-upload coverage 档位，恢复持久化配置并默认 `baseline`，但这不等于该 Host 驱动边界已经修复。

本轮公开证据边界与哈希见 [November 1 validation follow-up](../debugging/pr78-validation-20261101.md)。

D3D9 后端契约复核发现，自动化曾漏接 native 已支持的 `dxvk_legacy_d3d9`。现已恢复专用档位并放行该精确值；保留对模糊 `dxvk` 的拒绝。两架构 DXVK Legacy 已在独立 Linux 目录重建，新包完整性检查通过；设备上的 13 个 DXVK 运行库/清单文件已备份、替换并回读核验，未重置 prefix。新载荷 SHA-256 为 `138733418e6deb7b525606805a1fe0e9526a53567ec377e92b0904eebd899b3e`。专用套件仍失败，不能把 DLL 打包修复等同于 D3D9 可用，也不能用普通 `dxvk_legacy` 替代专用档位来规避。
