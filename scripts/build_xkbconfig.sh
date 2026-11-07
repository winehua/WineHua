#!/bin/bash
# build_xkbconfig.sh — 构建 xkeyboard-config (XKB 键盘布局数据) → sysroot-ext
# xkb 数据是架构无关的配置文件, Wine 键盘驱动初始化依赖
# 源码来自 thirdparty/xkeyboard-config (git submodule)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

XKBC_SRC="$ROOT/thirdparty/xkeyboard-config"
XKBC_BUILD="$BUILD_DIR/xkeyboard-config_build"
XKBC_INSTALL="$BUILD_DIR/xkeyboard-config_install"

log "=== 构建 xkeyboard-config (XKB 数据) ==="

# 已就绪则跳过
if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ] && [ -f "$SYSROOT_EXT_SHARE/X11/xkb/xkb.dtd" ]; then
    log "xkeyboard-config 已就绪, 跳过"
    exit 0
fi

if [ ! -f "$XKBC_SRC/meson.build" ]; then
    err "xkeyboard-config 源码未找到, 请先: git submodule update --init thirdparty/xkeyboard-config"
fi

# ── Meson 构建 (纯数据包, 无编译, 无需交叉编译工具链) ──
log "配置 xkeyboard-config..."
rm -rf "$XKBC_BUILD" "$XKBC_INSTALL"

meson setup "$XKBC_BUILD" "$XKBC_SRC" \
    --prefix=/usr \
    -Dxorg-rules-symlinks=false

# ninja compile: 生成 rules 文件 (rules-base, rules-evdev 等), 无实际二进制编译
log "编译 (生成 rules)..."
ninja -C "$XKBC_BUILD"

# 安装到 BUILD_DIR 下的临时目录 (不用 /tmp)
log "安装 xkeyboard-config..."
DESTDIR="$XKBC_INSTALL" meson install -C "$XKBC_BUILD"

# 复制到 sysroot-ext
# meson install 创建 X11/xkb → /usr/share/xkeyboard-config-2 (绝对路径) symlink
# 裸容器内 /usr/share/xkeyboard-config-2 不存在，cp -rL 无法解引用
# 改为直接复制实际数据；同时展开为普通目录 (打包不支持 symlink)
rm -rf "$SYSROOT_EXT_SHARE/X11/xkb" "$SYSROOT_EXT_SHARE/xkeyboard-config-2"
# cp -r source dest: dest 不存在时创建为 source 的副本
cp -rL "$XKBC_INSTALL/usr/share/xkeyboard-config-2" "$SYSROOT_EXT_SHARE/xkeyboard-config-2"
mkdir -p "$SYSROOT_EXT_SHARE/X11"
cp -rL "$XKBC_INSTALL/usr/share/xkeyboard-config-2" "$SYSROOT_EXT_SHARE/X11/xkb"
rm -rf "$XKBC_INSTALL"

log "xkeyboard-config → ${SYSROOT_EXT_SHARE}/X11/xkb/"
du -sh "$SYSROOT_EXT_SHARE/X11/xkb"
