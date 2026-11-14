# P0 X11 隔离构建补充

所有操作均在 `/workspace/WineHua-root/hgs-upstream-baseline`，不在原产品工作区执行。

## 工具与源码

先 source `build-tools-env.sh`。工具在工作区外层 `.hgs-build-tools/` 解包，不安装系统包；版本/归档 hash 见 `build-tools-lock.json`。原 SDK 不变。源码 SHA 以 `baseline-lock.json`、`dependencies.json` 为准；Wine 完整 worktree 为 025656df，Mesa 为 e975a707，wlroots 和 Xwayland 使用上游锁定版本及原有补丁。

上游构建补丁分别是 `upstream-build-fixes.patch`（Meson 实体库选择、native Wayland 缓存目录、宿主 FreeType、持久 host-ext 覆盖参数）和 `upstream-types-build-fixes.patch`（对齐已有 native API 类型声明）。运行时 C/C++、ArkTS 图形实现未修改。

## 输入准备

原缓存 sysroot-ext 的 guest 依赖和 native libffi/libxml2 安装前缀复用，并重写 pkg-config 的绝对前缀。guest 与 host 的 pkg-config 不能互用；本地 scanner 通过 `PKG_CONFIG_LIBDIR_FOR_BUILD`、`PKG_CONFIG_PATH_FOR_BUILD` 指向 host-tools。host 构建期间临时选 native libffi/libxml2 的 `.pc`，完成后恢复 guest `.pc`。

ARM64 pixman、libdrm、xkbcommon、Wayland、XCB/X11、virglrenderer、wlroots、Xwayland 已新构建。Wayland 协议生成头从 sysroot-ext 的 include/wayland-protocols 提供给 host-ext 的 include。纯数据协议 pc 没有包含头搜索路径，不能只依赖其 pkg-config 输出。

`x11-build-baseline.mk` 与仓库 Makefile 合用，显式传 ROOT。guest XCB/X11 → Wine；Mesa OpenGL/VirGL 与 Venus ICD 两个目标顺序执行，不能同时运行（共用模式相关 cross file）。Wine 宿主工具需要本地 FreeType 头/库，通过 `HGS_P0_HOST_FREETYPE_CFLAGS` 和 `HGS_P0_HOST_FREETYPE_LIBS` 覆盖；其余默认语义不变。

## 组装与打包

新 Wine/Mesa 之外，Box64、DXVK、VKD3D、Vulkan loader/诊断程序及 Mono 复用原缓存；详见 `build-artifacts.json`。smoke payload 由新 Wine 产物和上游测试源码重新构建，158 exe，版本 smoke-v2-031fe21849d3。xkbcomp 1.4.6 在宿主生成 9,420 B 的架构无关 XKM。

Hvigor 会清理应用根 build/，所以 native/host-ext 必须先复制到证据目录，并通过隔离 entry build-profile 的 CMake arguments 设置 `-DHGS_P0_HOST_EXT_ROOT=/workspace/WineHua-root/WineHua/build/hgs-p0/2026-11-06/x11-host-ext`。entry 外部 oh_modules 复用本地包，三个 native API 使用相对链接。隔离 root build profile 去掉 signingConfigs，仅 arm64-v8a。签名另用用户指定材料完成，不写进 build profile。

完整 Mono 包和无 Mono 图形配置分别留档。无 Mono 配置直接从完整 engine zip 按原 BUILD_WINE_MONO=0 的文件筛选派生，未重新编译任何库；其源码/证据边界和首次初始化结果见 README。后续从源码复现时应直接通过 BUILD_WINE_MONO=0 运行原 assemble，而非把这种派生步骤作为产品构建流程。

## 设备测试

用 `--payload /workspace/WineHua-root/WineHua/build/hgs-p0/2026-11-06/x11-smoke-payload`；每轮不同 run-id，每轮显式 `--display-route x11 --desktop-mode virtual`。job 的 params.env 同时声明 WINEHUA_DISPLAY_ROUTE=x11、DISPLAY=:0、WAYLAND_DISPLAY 为空。窗口/IME 子集与 Wayland 使用同样 8 项；图形出图按上游 displayroute 专用 jobs 测试，不能把 Wayland 的全矩阵成功率直接套到 X11。
