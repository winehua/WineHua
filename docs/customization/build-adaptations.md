# 构建适配与依赖引入

> 适用场景：排查某个 submodule 的构建失败、想知道"这个模块为什么在 thirdparty 里、做了什么改动"。
> 最后核实：2026-09-25
> 相关代码：`Makefile`（构建入口）、`scripts/build_deps.sh`、`scripts/build_gnutls.sh`、`scripts/build_gstreamer.sh`、`scripts/assemble.sh`（打包）
> 相关文档：[README.md](README.md)（模块一览）、[../build/guide.md](../build/guide.md)（构建流程）

这一篇收两类内容：对模块源码做的**构建级小改动**（不影响运行行为），以及**原样引入**但引入本身有故事的依赖。重度定制的模块在各专篇。

## glib 2.78.0（gstreamer 依赖）

**改动**：`meson.build` 的警告参数里去掉两项 `-Werror`：

- `-Wno-error=format-security`
- `-Wno-error=format-nonliteral`

**问题**：glib 自身代码在这两项上有告警，上游 CI 用 gcc/glibc 不开成 error；鸿蒙 NDK 的 clang 把它们当 error 处理，glib 编不过。

**思路**：只降这两项的等级（warning），不动其它 `-Werror`。改动停在 `meson.build` 一处，升级 glib 版本时重放即可。

**状态**：已暂存未提交（`git -C thirdparty/glib diff HEAD` 可见）。

## gstreamer 1.24.4（Wine 媒体管线后端）

**改动**：`subprojects/gst-plugins-bad/gst-libs/gst/meson.build` 注释掉 `subdir('cuda')`。

**问题**：gst-plugins-bad 的 cuda 库在 musl + 鸿蒙 NDK 下 C++ 链接失败，而 Wine 的媒体管线根本用不到它。

**思路**：整目录禁用而不是修链接——cuda 是纯可选组件，禁用不影响 winegstreamer 用到的插件集。

**状态**：已暂存未提交。

## pcre2 10.42（glib 依赖）

无实质改动。工作区的 `src/config.h.in` 差异是 autoconf 重新生成的注释引号风格（`` ` `` → `'`），可随时 checkout 丢弃。

## TLS 链：gnutls / nettle / libtasn1 / libunistring / gmp

全部**原样引入**，没有提交。引入它们本身是修复工作：

**问题**：Wine 的 HTTPS（schannel 后端）依赖 gnutls，鸿蒙系统没有提供；应用 HTTPS 全部握手失败（HTTP 正常）。早期还连带发现 box64 的 wrappedlibc 符号表缺 musl resolver 符号，guest 库加载就失败（该修复在 box64 fork，见 [box64.md](box64.md)）。

**思路**：把整条依赖链（`gmp → nettle(含 hogweed) → libtasn1 → libunistring → gnutls`）作为 submodule 源码引入，`scripts/build_gnutls.sh` 按序交叉编译进 `sysroot-ext`，Wine 的 schannel 链接它；`scripts/assemble.sh` 把 `libgnutls.so.30` 等随 HAP 打包。gmp/nettle/libtasn1/libunistring 是为编译 gnutls 带进来的间接依赖，本身不被 Wine 直接使用。

**注意**：gnutls tarball 走 gnupg.org 官方源，国内网络直连很慢，构建前按 [../build/env.md](../build/env.md) 配代理。

## 原样引入的其余模块

这些模块没有任何提交，列出引入目的备查（版本基线见 [README.md](README.md) 一览表）：

| 模块 | 谁依赖它 |
|---|---|
| wayland、wayland-protocols | winewayland.drv 的构建：wayland-scanner 从协议 XML 生成代理代码 |
| vkd3d-proton | D3D12→Vulkan（`vkd3d_limited_500k` 档），适配做在环境变量注入层，代码零改动 |
| freetype | win32u 字体光栅化 |
| libxkbcommon、xkeyboard-config | winewayland.drv 键盘映射（键码→Windows 虚拟键） |
| libdrm | mesa venus/virgl 的构建依赖（鸿蒙无 DRM，只用头文件与辅助代码） |
| libxml2 | Wine 构建的可选依赖 |
| libffi | glib 的 GObject 依赖 |
