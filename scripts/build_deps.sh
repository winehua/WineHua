#!/bin/bash
# build_deps.sh — 编排所有交叉编译依赖 (freetype → wayland → xkbcommon)
# 所有产物安装到 build/sysroot-ext/，不污染 OHOS SDK
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建模拟层交叉编译依赖 (Wine用, $TARGET) → sysroot-ext ==="

# 按依赖链顺序执行 (模拟层依赖, 架构 = WINE_ARCH 推导的 $TARGET)
bash "$SCRIPT_DIR/build_freetype.sh"
bash "$SCRIPT_DIR/build_libffi.sh"
bash "$SCRIPT_DIR/build_wayland.sh"
bash "$SCRIPT_DIR/build_xkbcommon.sh"
# GnuTLS 链 (Wine schannel TLS 后端: gmp/nettle/libtasn1/libunistring/gnutls)
bash "$SCRIPT_DIR/build_gnutls.sh"
# GStreamer 链 (Wine winegstreamer 后端: pcre2/glib/gstreamer/gst-plugins-base)
bash "$SCRIPT_DIR/build_gstreamer.sh"
# XKB 键盘布局数据 (xkeyboard-config, Wine 键盘驱动依赖, 架构无关)
bash "$SCRIPT_DIR/build_xkbconfig.sh"

# Guest GPU Mesa/VirGL 库 (输出到 build/guest_gfx/$GUEST_ARCH/)
# Wine 标准 OpenGL 路径 (opengl32 → winewayland.drv → compositor EGL) 不需要此 bundle
# 设置 BUILD_GUEST_GFX=1 启用 (需要 thirdparty/mesa + libdrm + wayland-protocols >= 1.38)
if [ "${BUILD_GUEST_GFX:-0}" = "1" ]; then
    if [ ! -d "$ROOT/thirdparty/mesa" ] || [ ! -d "$ROOT/thirdparty/libdrm" ] \
       || [ ! -d "$ROOT/thirdparty/wayland-protocols" ]; then
        err "BUILD_GUEST_GFX=1 但 thirdparty/mesa, libdrm 或 wayland-protocols 缺失"
    fi
    log "=== 构建 guest_gfx (Mesa/VirGL) 从源码 ==="
    # 确保 stub hilog 头文件可用 (OHOS Native SDK 不含 hilog/log.h)
    mkdir -p "$SYSROOT_EXT_INC/hilog"
    [ -f "$SYSROOT_EXT_INC/hilog/log.h" ] || cat > "$SYSROOT_EXT_INC/hilog/log.h" << 'HILEOF'
#ifndef STUB_HILOG_LOG_H
#define STUB_HILOG_LOG_H
#include <stdio.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { LOG_DEBUG=3, LOG_INFO=4, LOG_WARN=5, LOG_ERROR=6, LOG_FATAL=7 } LogLevel;
#define LOG_APP 0
static inline int HiLogPrint(unsigned int d, unsigned int l, unsigned int t, const char *f, ...)
    __attribute__((format(printf,4,5)));
static inline int HiLogPrint(unsigned int d, unsigned int l, unsigned int t, const char *f, ...) {
    (void)d;(void)t; va_list va; va_start(va,f); vfprintf(stderr,f,va); va_end(va); return 0; }
#ifdef __cplusplus
}
#endif
#endif
HILEOF
    # NATIVE_ARCH 是 Native 层架构 (arm64-v8a/x86_64), 决定 TARGET/WINE_ARCH;
    # GUEST_ARCH (aarch64/x86_64) 只决定输出目录, 由 make export 或脚本推导.
    NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPT_DIR/build_ohos_guest_gfx.sh"
else
    log "guest_gfx: SKIP (设置 BUILD_GUEST_GFX=1 启用 Mesa/VirGL 图形测试 bundle)"
fi

# Guest Vulkan runtime: x86_64 Vulkan Loader + Mesa Venus ICD + deterministic
# offscreen probe.  This deliberately remains separate from Wine Vulkan so B1
# can diagnose the guest/host transport without Wine or Win32 WSI in the path.
if [ "${BUILD_GUEST_VULKAN:-0}" = "1" ]; then
    [ "${BUILD_GUEST_GFX:-0}" = "1" ] || \
        err "BUILD_GUEST_VULKAN=1 requires BUILD_GUEST_GFX=1 (Mesa Venus ICD)"
    log "=== 构建 guest_vulkan (Loader + Venus ICD + smoke) ==="
    NATIVE_ARCH="$NATIVE_ARCH" GUEST_ARCH="$GUEST_ARCH" bash "$SCRIPT_DIR/build_ohos_guest_vulkan.sh"
else
    log "guest_vulkan: SKIP (设置 BUILD_GUEST_VULKAN=1 启用 Venus Vulkan runtime)"
fi

# Native compositor 依赖 (wayland-server for HAP) 在 build.sh 中按架构单独调用:
#   bash scripts/build_native.sh

# Wine Mono (.NET 运行时) — 预编译 MSI, 默认启用 (增加 ~80MB)
# 设置 BUILD_WINE_MONO=0 跳过
if [ "${BUILD_WINE_MONO:-1}" = "1" ]; then
    # 必须与 mscoree 侧期望一致: appwiz.cpl addons.c MONO_VERSION /
    # mscoree_private.h WINE_MONO_VERSION 均为 11.1.0. install_addon
    # 按 addon->file_name 精确匹配, 版本不一致 → 找不到 msi → 弹框卡死
    WINE_MONO_VER="11.1.0"
    WINE_MONO_MSI="wine-mono-${WINE_MONO_VER}-x86.msi"
    WINE_MONO_URL="https://dl.winehq.org/wine/wine-mono/${WINE_MONO_VER}/${WINE_MONO_MSI}"
    WINE_MONO_SHA256="deb0341431f8260b209fff6bc79ddcc5414b97f8e9236ab9fbdca4ce59e0a9b9"
    # mono msi 架构无关, 放架构无关路径 (不依赖 WINE_ARCH, 跨方案/跨架构共享)
    WINE_MONO_DIR="$BUILD_DIR/wine-mono"
    WINE_MONO_PATH="$WINE_MONO_DIR/$WINE_MONO_MSI"
    if [ ! -s "$WINE_MONO_PATH" ] || ! echo "$WINE_MONO_SHA256  $WINE_MONO_PATH" | sha256sum -c --status; then
        log "=== 下载 Wine Mono ${WINE_MONO_VER} ==="
        mkdir -p "$WINE_MONO_DIR"
        WINE_MONO_TMP="$WINE_MONO_PATH.download"
        rm -f "$WINE_MONO_TMP"
        if command -v curl >/dev/null 2>&1; then
            curl --fail --location --retry 3 --output "$WINE_MONO_TMP" "$WINE_MONO_URL" || \
                err "Wine Mono 下载失败"
        elif command -v wget >/dev/null 2>&1; then
            wget -O "$WINE_MONO_TMP" "$WINE_MONO_URL" || err "Wine Mono 下载失败"
        elif command -v python3 >/dev/null 2>&1; then
            python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' \
                "$WINE_MONO_URL" "$WINE_MONO_TMP" || err "Wine Mono 下载失败"
        else
            err "无 curl/wget/python3, 无法准备默认 Wine Mono 运行时"
        fi
        echo "$WINE_MONO_SHA256  $WINE_MONO_TMP" | sha256sum -c --status || \
            err "Wine Mono SHA-256 校验失败"
        mv "$WINE_MONO_TMP" "$WINE_MONO_PATH"
    fi
    echo "$WINE_MONO_SHA256  $WINE_MONO_PATH" | sha256sum -c --status || \
        err "Wine Mono 产物缺失或损坏"
    log "Wine Mono → $WINE_MONO_PATH"
else
    log "Wine Mono: SKIP (设置 BUILD_WINE_MONO=1 启用 .NET 运行时)"
fi

log "模拟层依赖就绪: $SYSROOT_EXT"
echo ""
find "$SYSROOT_EXT" -type f | sort
