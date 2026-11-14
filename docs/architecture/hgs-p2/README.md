# HGS P2 X11 服务核心

实现位于 WineHua 工作区的 HGS 子模块 `hgs/main`，基于已发布的 HGS P1 提交 `6f71911a7cec0a288d4afcecceae7f8fe07e133d`。本轮代码在本地验证，尚未发布。P2 的首批实现接管 X11 监听、Xwayland 子进程、逻辑输出、窗口注册及键鼠路由。后续工作包括 XIM 和 OHOS 窗口呈现。

## 启动链

1. 宿主选择 `x11` 或 `both`，并提供 XKB 规则及预编译 XKM。HGS 检查资源后，在私有目录中生成会话 Xauthority 和对应 display 的 XKM。
2. HGS 原子绑定标准本地 abstract socket 并创建独立的 wlroots headless output、pixman renderer 和 seat。它通过 NCP 传递监听、Wayland、XWM、就绪和控制 fd。Xwayland 只在子进程中获得 XDG_RUNTIME_DIR 等环境。
3. Xwayland 通知 displayfd 后，HGS 等待 wlroots XWM 就绪才公布 DISPLAY、XAUTHORITY。应用自行启动普通 X 客户端。X11 backend 不调用 Wine 的启动、帧 broker 或旧 Wayland 单例。
4. X 窗口 map/unmap 进入公共 registry。按 HGS WindowId 投递输入，在 wlroots 线程核对 XID 和对象代次。最后一个窗口关闭后监听仍保持。
5. stop 撤销窗口与监听，关闭自己启动的子进程通道并等待退出通知，清理私有认证文件。运行时 Xwayland 失效时发出 `backend-disconnected` 并撤销 X11 端点。

构建时开启 `HGS_BUILD_X11`，用显式 SDK 前缀提供 wlroots 0.20.2、Xwayland 24.1.13 及 ARM64 依赖。最小宿主使用单一 Wayland SONAME 提供者。构建和接入详见 HGS 的 `docs/integration/x11.md`。

## 验证记录

原始证据和签名 HAP 位于 `/workspace/WineHua-root/WineHua/build/hgs-p2/`。最小宿主借用本地 WineHua 测试包身份签名，未打包 Wine 引擎、`libentry.so` 或 BetterSH 代码。签名使用已授权的既有本地材料。

| 验证 | 结果 |
|---|---|
| HGS 主机测试 | PASS，包含双后端原子回滚、X11 模拟断连后 Wayland 保持 Ready、Unix socket 竞争及重启 |
| WineHua 主机回归 | `make test` 通过，覆盖现有 Wayland 窗口、输入与渲染相关主机用例 |
| ARM64 native 构建 | X11 开启与关闭配置均通过 |
| 运行库依赖闭包 | 首次真机超时定位到 Xwayland 缺 libfreetype.so.6，补齐后启动通过。打包脚本会报告未解析的 NEEDED 库 |
| 最小宿主 X11 | 最终包的 X11 单协议模式通过两轮普通 Xlib 客户端，验证拒绝无 cookie 连接、授权连接、建窗、绘图像素读回、鼠标与 A 键按下释放 |
| 最小宿主重连 | 同 epoch 复用相同 XID 时，客户端 ID、窗口 ID 和对象代次均更新 |
| 监听回收 | 修正 NCP 父侧 fd 后，真机同一进程 stop 再 start 重绑 `:0` |
| both | 最终包报告两个就绪端点，X11 客户端通过。混合 Wayland/X11 窗口交互属于后续验收 |
| 异步启停与启动回滚 | 最终包真机通过。缺失 XKB 资源时 `both` 返回 `invalid-config/x11-resources`，撤销已开的 Wayland 监听，不公布端点。下一次启动成功，停止后清理 Xauthority |

最终 HAP 为 `minimal-x11-final-signed.hap`，设备报告 `minimal-final-report.log` 以 `P2 TEST PASS` 结束。复测后已覆盖安装 P1 WineHua 签名包并确认入口启动成功，见 `restore-p1-install.log` 和 `restore-p1-start.log`。

当前绘图验证通过 XGetImage 读回，呈现仍是 headless。X11 的 OHOS surface 接入、XIM、Wine 外部帧和两套窗口的混合焦点均有待继续迁移。P0 记录的 Wine GPU 帧同步与 Vulkan 问题不由本轮测试判定。
