# WineHua 上游同步与 PR 审查

## 合并基线

- PR 基分支为 `winehua/WineHua:master`，本轮合入 `b5f8d516ec6508ceeed6a370f804b5ce5c479c21`。
- `feature/using-hgs` 合并前位于 `82af98cce`，与上游的共同祖先是 `3eecbd732`。上游有 314 个提交需要进入分支历史。此前 11 月 6 日的 fetch 记录为 `a001e6d0c`，之后只有 Wine 子模块的 X11 selection flush 修复。
- 原始三方合并有 42 个冲突，28 个是宿主已删而上游仍在修改的文件，14 个是两端内容冲突。合并保留双方历史。审查分支差异用 `git diff upstream/master...feature/using-hgs`，审查合并取舍可对合并提交用 `git show --remerge-diff`。
- `thirdparty/wine` 和其他第三方指针与该上游基分支一致。HGS 依赖 `28cff5a`，它包含 WHGP v2、Wayland 输入法和指针锁改动，以及 X11 窗口宿主和呈现接线。

## 按功能审查

| 领域 | 上游变化 | 合并后的落点 |
| --- | --- | --- |
| 进程和 Wine 启动 | 首启改用 `winehua_keep.exe --boot-once`，二启由首个 PE 进程触发 wineboot。增加 zHome 持久化、失败重试和 Wine 调试环境透传。 | 保留首启完成标记和超时判据。WineHua 的 broker 从 HGS 会话取得实际 `DISPLAY`、`XAUTHORITY`、`WAYLAND_DISPLAY` 和运行目录。普通程序、桌面和内部分派共享这一套端点。 |
| 显示路线 | WineHua 内置的 Wayland 与 wlroots/Xwayland 通过独立 NAPI 和全局路线切换。 | 两个后端由 HGS 管理。保留 `winehua.displayRoute` Want 键和旧的 `winehua.hgsX11Probe`，同时接受 `winehua.displayProtocol`。首次加载时读取已有 `display_route` 偏好。设置页走停止并重启 Wine 引擎的路径。 |
| X11 窗口 | 有专用桌面画布、Fusion 窗、popup、modal 装饰和位置回写。 | X11 使用 HGS 公共窗口宿主处理窗口和 Surface，WineHua 订阅 XIM 目标以及虚拟桌面首帧。Wayland 延续原有 WineHua 窗口管理。旧画布、标题栏和几何行为需要按下文真机矩阵核对。 |
| Wayland 输入法和指针 | 重新聚焦用上一份非空光标矩形，键盘收起后允许下一次激活，桌面壳指针锁不触发宿主隐藏。 | 在 HGS Wayland 后端实现。矩形跟随 surface 销毁清理，WineHua 调用 HGS 的 `imeRearm`。X11 退格和文本走当前 HGS 会话的键盘与 XIM。 |
| 手柄和音频 | WHGP v2 统一坐标，摇杆整组更新，音频按端点混音并在来电结束后恢复，Wine 宣告 float32 WASAPI 格式。 | 音频代码保留上游版本。手柄移动到 HGS 对应目录，保留旧 `controllerSetAxis`，新增 `controllerSetStick`、`controllerSetTrigger`。WineHua 测试直接使用 HGS 协议头。旧 WHGP v1 Wine guest 需要配套旧 HGS 或重建。 |
| GPU 呈现 | GraphicsBroker 以生产窗身份判断重复绑定，并用单个 surface 的 GL/Vulkan 类型选择 presenter。 | 保留 Broker 逻辑。Wayland 通过 HGS 的宿主接缝传递 surface 类型，X11 的 provider 通过 XID 和 PID 绑定。两条路线的 GPU 显示序列记录协议和 surface key。 |
| 构建和自动化 | 上游更新 Mesa、Wayland、XKB、wlroots、Xwayland 及构建脚本，增加多路线 smoke 用例。 | 保留依赖构建链。HGS HAR 按 ABI 读取宿主前缀，打包 HGS 格式的 X11 键盘资源。X11 smoke 的窗口定位、键鼠和文本改走 HGS API。旧 xclient 台架和画布预览专用用例还需迁到 HGS 独立宿主。 |

## 验证记录和审查清单

- WineHua 的 16 个 C++ 主机测试程序通过。WHGP 验证读取 HGS 源码和 `6559a57a0` 的 Wine 文件通过。HGS 主机服务与 socket 测试通过。
- 子 agent 用 P2/P3 SDK 和 P5 已分阶段准备的运行库及 Wine 载荷，重新构建 ARM64 release HAP。检查包内有 `libhgs.so`、`libhgs_xwayland_child.so`、`libhgs_xim_child.so` 和 `hgs-x11-res.zip`。这次编译使用隔离的构建输入，正式产物仍需以 `6559a57a0` 从头重建 Wine guest、签名和安装。构建 XIM 时可传 `WINEHUA_HGS_XIM_SDK` 指定 OHOS SDK 前缀。
- 真机待验证的组合包括 Wayland 和 X11 各自的首启与二启、切换与重复启停、不同桌面模式、popup/modal 关联和系统标题栏、X11 剪贴板、中文输入、WHGP 手柄与音频中断恢复。X11 的独立窗口宿主与上游旧画布不共用窗口事件及坐标模型，重点复查缩放、相对指针、宿主拖动到 guest 的位置回写及相关 smoke。
- X11 Wine 虚拟桌面承载为 HGS 管理的单个 X11 窗口。HGS 的跨协议共享 DesktopRoot、跨协议剪贴板和拖放仍属后续工作。当前测试证据不能覆盖这些能力。
