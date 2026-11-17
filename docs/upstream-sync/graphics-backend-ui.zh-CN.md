# 新增图形后端测试入口

在 WineHua 的现有侧栏“图形后端”增加 DXVK9（32 位）和 Zink，与 VirGL/WineD3D、DXVK 1.10.3、DXVK 2.6.2、VKD3D 2.6（500K）平等选择。原首次启动默认及用户已有偏好不变。Zink 使用 native 已有优化配置，界面不增加实验调优参数。

三处后端白名单统一调用 GraphicsBackendPolicy，避免选项在界面、冷启动或引擎服务中退回 WineD3D。选择写入现有 wine_settings 的 preferences，并兼容原 AppStorage/PersistentStorage 键；EntryAbility 先恢复持久化偏好，再应用只对本次有效的 Want 覆盖。独立调整 DXVK 档位不覆盖 DXVK9、Zink 或 VKD3D 模式。读取偏好失败仍可使用原默认启动。

设置说明明确要求重启 Wine 后生效。先保存游戏，再使用现有“重启 Wine”按钮；不会自动关闭运行中的游戏。设置为整个 Wine 环境选择，本轮没有增加游戏库或每游戏配置功能。

验证入口：容器内 `make runtime-ui-check`。测试执行真实 WineEnvService 和 EntryAbility 的 ArkTS 代码，覆盖六档设置、冷启动恢复、临时 Want 优先级且不污染已保存值、独立 DXVK 档位以及偏好服务不可用。ArkTS 与最终 HAP 的结果保存在本任务调试包记录；不沿用历史游戏性能作为本包结果。

产品边界守卫仅放行 Index、WineEnvService、EntryAbility 及新 GraphicsBackendPolicy 四个 ArkTS 文件；AppScope、权限、资源、其他页面与原 NAPI 接口继续受保护。
