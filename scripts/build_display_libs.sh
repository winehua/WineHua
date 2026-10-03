#!/bin/bash
# build_display_libs.sh — host 侧显示基础库: pixman + libdrm + xkbcommon → host-ext/<NATIVE_ARCH>
# 侧别: 三者均 host 组件 (wlroots/Xwayland 消费) — 架构侧别纪律见 .claude/rules/build-and-log.md
# (guest 侧 xkbcommon 走 build_xkbcommon.sh; wayland-protocols 数据走 build_wayland.sh)
# 用法: make deps 自动调用; 也可独立执行 bash scripts/build_display_libs.sh
# 版本钉死 + sha256 校验; 产物断言失败即非零退出
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

DL="$BUILD_DIR/downloads"
mkdir -p "$DL"

fetch_and_unpack() {  # $1=url  $2=sha256  $3=解压后目录名
    local tgz="$DL/$(basename "$1")"
    if [ ! -f "$tgz" ]; then
        log "--- 下载 $(basename "$1") ---"
        curl -fsSL -o "$tgz" "$1"
    fi
    echo "$2  $tgz" | sha256sum -c - || { err "sha256 不匹配: $tgz"; }
    [ -d "$BUILD_DIR/$3" ] || tar -C "$BUILD_DIR" -xf "$tgz"
}

# host 侧 pc 查询: HOST_EXT 优先, SDK sysroot 次之, guest 数据类兜底
host_pc() { PKG_CONFIG_LIBDIR="$HOST_EXT_PC:$SYSROOT/usr/lib/pkgconfig:$SYSROOT_EXT_PC" pkg-config "$@"; }

# 版本下限断言: sort -V 数值语义 (教训: [ "1.13.2" \> "1.7.99" ] 是字典序, 会误判)
assert_min_ver() {  # $1=当前  $2=下限  $3=名称
    local cur="$1"
    [ -n "$cur" ] || err "$3 版本不可解析"
    [ "$(printf '%s\n' "$2" "$cur" | sort -V | head -1)" = "$2" ] || err "$3 版本不达标: $cur (需 ≥ $2)"
}

# ── pixman (host, wlroots pixman 渲染器 + Xwayland, 门槛 ≥0.43.0) ──
PIXMAN_VER=0.46.4
PIXMAN_SHA=d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c
if [ ! -f "$HOST_EXT_PC/pixman-1.pc" ]; then
    fetch_and_unpack "https://www.cairographics.org/releases/pixman-${PIXMAN_VER}.tar.gz" \
        "$PIXMAN_SHA" "pixman-${PIXMAN_VER}"
    # 构建目录带版本后缀: 升级时旧 meson 缓存不可复用 (同 build_wayland.sh 教训)
    meson_host_build "$BUILD_DIR/host_pixman_build_${PIXMAN_VER}" "$BUILD_DIR/pixman-${PIXMAN_VER}" \
        --prefix="$HOST_EXT_USR" --libdir=lib \
        -Dtests=disabled -Ddemos=disabled
    ninja -C "$BUILD_DIR/host_pixman_build_${PIXMAN_VER}" install
fi
assert_min_ver "$(host_pc --modversion pixman-1 2>/dev/null)" "0.43.0" "host pixman"

# ── libdrm (host, wlroots 无条件依赖——像素格式头, 门槛 ≥2.4.129; 芯片后端全关) ──
DRM_VER=2.4.134
DRM_SHA=ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95
# 守卫用 host-ext 文件存在性而非 pkg-config 查询: SDK sysroot 自带 libdrm.pc(2.4.120)
# 会短路 --exists → 误跳过 2.4.129 门槛的自建 (实测踩坑)
# 注意: 此处**不得**试图给 virglrenderer 供 gbm (libdrm ≥2.4.121 已无 gbm
# 选项, 它拆去了 mesa)。virgl 的 meson 视野已在 build_native.sh 收窄到
# epoxy —— 豁免分支确定性生效, 与本目录是否装 libdrm 无关 (2026-10-04
# 毁灭性重建实证)。
if [ ! -f "$HOST_EXT_PC/libdrm.pc" ]; then
    fetch_and_unpack "https://dri.freedesktop.org/libdrm/libdrm-${DRM_VER}.tar.xz" \
        "$DRM_SHA" "libdrm-${DRM_VER}"
    meson_host_build "$BUILD_DIR/host_drm_build_${DRM_VER}" "$BUILD_DIR/libdrm-${DRM_VER}" \
        --prefix="$HOST_EXT_USR" --libdir=lib \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
        -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dtegra=disabled \
        -Dvc4=disabled -Detnaviv=disabled -Dfreedreno=disabled -Dtests=false
    ninja -C "$BUILD_DIR/host_drm_build_${DRM_VER}" install
fi
assert_min_ver "$(host_pc --modversion libdrm 2>/dev/null)" "2.4.129" "host libdrm"

# ── xkbcommon host 侧 (门槛 ≥1.8, wlroots 0.20.2 要求; 源码 = thirdparty/libxkbcommon, 与 guest 共用) ──
XKBC_SRC="$ROOT/thirdparty/libxkbcommon"
XKBC_VER=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$XKBC_SRC/meson.build" | head -1)
[ -n "$XKBC_VER" ] || err "无法从 $XKBC_SRC/meson.build 解析 version"
if [ ! -f "$HOST_EXT_PC/xkbcommon.pc" ]; then
    meson_host_build "$BUILD_DIR/host_xkbcommon_build_${XKBC_VER}" "$XKBC_SRC" \
        --prefix="$HOST_EXT_USR" --libdir=lib \
        -Denable-x11=false -Denable-wayland=false \
        -Denable-xkbregistry=false -Denable-docs=false \
        -Denable-tools=false
    ninja -C "$BUILD_DIR/host_xkbcommon_build_${XKBC_VER}" install
fi
assert_min_ver "$(host_pc --modversion xkbcommon 2>/dev/null)" "1.8.0" "host xkbcommon"

# ── 侧别架构断言: host 产物必须是 NATIVE_TARGET 架构 ──
check_arch() {  # $1=.so 路径
    local m want
    m=$("$OHOS_SDK/native/llvm/bin/llvm-readelf" -h "$1" | awk '/Machine:/{print $2}')
    case "$NATIVE_TARGET" in
        aarch64-linux-ohos) want="AArch64" ;;
        x86_64-linux-ohos)  want="Advanced" ;;   # readelf 对 x86-64 输出 "Advanced Micro Devices X86-64"
        *) err "未知 NATIVE_TARGET: $NATIVE_TARGET" ;;
    esac
    [ "$m" = "$want" ] || err "架构侧别错误 (期望 $want): $1 → $m"
}
check_arch "$HOST_EXT_LIB/libpixman-1.so"
check_arch "$HOST_EXT_LIB/libdrm.so.2"
check_arch "$HOST_EXT_LIB/libxkbcommon.so.0"

log "host display libs OK ($NATIVE_ARCH): pixman=$(host_pc --modversion pixman-1) libdrm=$(host_pc --modversion libdrm) xkbcommon=$(host_pc --modversion xkbcommon)"
