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

if [ ! -f "$XKBC_SRC/meson.build" ]; then
    err "xkeyboard-config 源码未找到, 请先: git submodule update --init thirdparty/xkeyboard-config"
fi

# A source tree copied through a Windows/WSL share can turn Git symlinks into
# regular files containing their link targets.  xkeyboard-config's generator
# resolves options through those links, so fail closed unless a regular file is
# exactly the tracked target and can be restored without touching a real edit.
tracked_source_symlink_paths() {
    git -c safe.directory="$XKBC_SRC" -C "$XKBC_SRC" ls-files -s | \
        awk '$1 == "120000" {print $4}'
}

repair_source_symlinks() {
    local path target link actual
    command -v git >/dev/null 2>&1 || err "xkeyboard-config 需要 git 来校验源码符号链接"

    while IFS= read -r path; do
        [ -n "$path" ] || continue
        link="$XKBC_SRC/$path"
        target="$(git -c safe.directory="$XKBC_SRC" -C "$XKBC_SRC" show ":$path")" || \
            err "无法读取 xkeyboard-config 符号链接索引: $path"

        if [ -L "$link" ]; then
            [ "$(readlink "$link")" = "$target" ] || \
                err "xkeyboard-config 符号链接目标被修改: $path"
            continue
        fi

        [ -f "$link" ] || err "xkeyboard-config 符号链接缺失: $path"
        actual="$(cat "$link")"
        [ "$actual" = "$target" ] || \
            err "xkeyboard-config 符号链接替代文件被修改: $path"
        rm -f "$link"
        ln -s "$target" "$link"
        xkbc_source_symlink_repaired=1
        log "恢复 xkeyboard-config 符号链接: $path -> $target"
    done < <(tracked_source_symlink_paths)
}

packaged_symlinks_ready() {
    local path link

    while IFS= read -r path; do
        [ -n "$path" ] || continue
        link="$SYSROOT_EXT_SHARE/X11/xkb/$path"
        [ -f "$link" ] && [ ! -L "$link" ] || return 1
    done < <(tracked_source_symlink_paths)
}

materialize_packaged_symlinks() {
    local path source destination_root destination

    while IFS= read -r path; do
        [ -n "$path" ] || continue
        source="$XKBC_SRC/$path"
        [ -L "$source" ] || err "xkeyboard-config 源符号链接未恢复: $path"
        for destination_root in \
            "$SYSROOT_EXT_SHARE/xkeyboard-config-2" \
            "$SYSROOT_EXT_SHARE/X11/xkb"; do
            destination="$destination_root/$path"
            mkdir -p "$(dirname "$destination")"
            cp -L "$source" "$destination"
            [ -f "$destination" ] && [ ! -L "$destination" ] || \
                err "xkeyboard-config 符号链接实体化失败: $path"
        done
    done < <(tracked_source_symlink_paths)
}

xkbc_source_symlink_repaired=0
repair_source_symlinks

# The layout tree is only reusable when it contains the generated rules and
# all source links were materialized into regular rawfile-safe data files.
if [ "$xkbc_source_symlink_repaired" -eq 0 ] && \
   [ -f "$SYSROOT_EXT_SHARE/X11/xkb/xkb.dtd" ] && \
   [ -f "$SYSROOT_EXT_SHARE/X11/xkb/rules/evdev" ] && \
   [ -f "$SYSROOT_EXT_SHARE/X11/xkb/keycodes/evdev" ] && \
   [ -f "$SYSROOT_EXT_SHARE/X11/xkb/symbols/us" ] && \
   [ -z "$(find "$SYSROOT_EXT_SHARE/X11/xkb" -type l -print -quit)" ] && \
   packaged_symlinks_ready; then
    log "xkeyboard-config 已就绪, 跳过"
    exit 0
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
# cp -rL source dest: 展开内部符号链接，避免 rawfile 打包保留失效链接。
mkdir -p "$SYSROOT_EXT_SHARE/X11"
cp -rL "$XKBC_INSTALL/usr/share/xkeyboard-config-2" "$SYSROOT_EXT_SHARE/xkeyboard-config-2"
cp -rL "$XKBC_INSTALL/usr/share/xkeyboard-config-2" "$SYSROOT_EXT_SHARE/X11/xkb"
materialize_packaged_symlinks
rm -rf "$XKBC_INSTALL"

log "xkeyboard-config → ${SYSROOT_EXT_SHARE}/X11/xkb/"
du -sh "$SYSROOT_EXT_SHARE/X11/xkb"
