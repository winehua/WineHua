手机游戏需要按程序保存主文件、参数、环境、图形后端和桌面分辨率。此变更在 main-ui 应用库加入「游戏配置」，启动时复用公共 Wine 启动接口；分辨率变更先提示保存并重启，默认配置保留原生 CPU 策略。

这是 #78 的产品线配套分支。公共输入、图形和构建修复通过 merge 从技术分支带入；独立游戏库页面、Ability 和配置模型已移出技术分支。保留 main-ui 原有应用库、悬浮桌面、主题、PC 缩放和完整键盘，并可切换至可编辑游戏按键；面板切换、隐藏或销毁时释放输入。

本轮修复了真实游戏有声音却黑屏/停在首帧的问题：默认 Vulkan invalidate 恢复实际区间读回，避免无关 fence 的整块读回覆盖 Guest 待发布数据。MewGenics 与 Darkest Dungeon 均已进入主菜单，用户确认动画恢复；游戏库“保存并启动”和 Home 后返回也有实机证据。保存名称后卡片标题不刷新的问题已修复并实机回读，测试名称已恢复。

验证（2026-11-02）：

- 当前产品主机回归 **38/38**，含实际 renderer/dispatcher 提取测试及两个旧逻辑负对照。
- Native/ArkTS 签名构建、CRC、运行库/DXVK 清单与哈希检查通过，设备接受覆盖安装；版本 1.0.31。
- 产品 HAP SHA-256：`4db33c6d44ff79eeea13f2e143b163ec84971fd1640eb90efe992bad3fdc4b8d`。
- 完整重建 Wine payload SHA-256：`48c0ec3b476948359adedf51359a76216426d1ac48424253c0b74ed55935c497`。未卸载、重置 prefix 或清除游戏/存档。
- 同一产品包单轮设备结果：core **4/4**、DXVK **3/3**、D3D12 **2/3**、DXVK D3D9 **3/4**、Wine Vulkan **3/4**。
- 受控 Win32 EDIT 实机确认完整键盘 A/Tab、游戏按键 Esc/Enter 的成对 Guest WM_KEYDOWN/WM_KEYUP 和扫描码；A 写入字符 a。此项不代表完整输入/IME 已通过。

**保持 Draft，发布验收未通过。** D3D12 1000 帧用例在第 242 帧等待 fence 失败；x64 D3D9 呈现 50 帧但未产生固定帧截图；x86 Vulkan sampled-only 保留首个 compute 的 `0xdeadbeef`。失败全部保留，没有重跑掩盖。完整输入/IME、布局持久化、多指所有权、长时间游戏和其他设备仍未全部验证。

完整证据范围、产物身份及历史失败见[产品验收记录](main-ui-integration-status.md)。GitHub Actions 的 fork 工作流仍需要上游维护者批准，提交者没有该权限。
