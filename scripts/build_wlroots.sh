#!/bin/bash
# build_wlroots.sh — wlroots 0.20.2 钉版最小集合构建 (host 侧) → host-ext/<NATIVE_ARCH>
# 侧别: wlroots 是 host 组件 (显示路线合成器内核) — 纪律见 .claude/rules/build-and-log.md
# 构建集合 = spec §3: -Dauto_features=disabled -Dxwayland=enabled, 静态库
# 消费依赖: wayland-server(entry/libs aarch64, build_native.sh) + host-ext pixman/drm/xkbcommon
#          + xcb 栈(host-ext) + wayland-protocols 数据(sysroot-ext/share)
# 用法: bash scripts/build_wlroots.sh (NATIVE_ARCH 经 env.sh 默认 arm64-v8a)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WLR_SRC="$ROOT/thirdparty/wlroots"
WLR_VER=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$WLR_SRC/meson.build" | head -1)
[ -n "$WLR_VER" ] || err "无法从 $WLR_SRC/meson.build 解析 version"
[ "$WLR_VER" = "0.20.2" ] || err "wlroots 钉版漂移: $WLR_VER (spec 钉 0.20.2)"

# 静态库/头/pc 均带项目版本号 (上游 pkg.install 实名), 消费方用 pkg-config wlroots-0.20
WLR_PC_NAME="wlroots-0.20"
OUT_LIB="$HOST_EXT_LIB/libwlroots-0.20.a"
OUT_INC="$HOST_EXT_INC/wlroots-0.20"

# ── 前置: host 侧 wayland pc (build_native.sh emit_wayland_host_pc 产出) ──
# 没有 host pc 时 meson 的 pc wrapper 会静默解析到 sysroot-ext 的 GUEST 侧 (x86_64)
# wayland.pc——静态 .a 构建不暴露问题, 试链接时才炸。此处断言防侧别错乱。
[ -f "$HOST_EXT_PC/wayland-server.pc" ] && [ -f "$HOST_EXT_PC/wayland-client.pc" ] \
    || err "host 侧 wayland pc 缺失, 先跑: NATIVE_ARCH=$NATIVE_ARCH bash scripts/build_native.sh"

# ── xwayland.pc (暂存复刻版, T6 由真实 xserver 构建接管) ──
# wlroots -Dxwayland=enabled 需要 xwayland.pc, 只消费两类变量:
#   xwayland=<二进制路径> (server.c:129,472 仅作字符串/运行时 access 检查)
#   have_listenfd / have_terminate_delay / have_no_touch_pointer_emulation /
#   have_force_xrandr_emulation (server.c:54-92 仅决定启动 argv 是否附加对应 flag)
# 以下内容逐行复刻 xwayland-24.1.13 hw/xwayland/meson.build pkgconfig.generate
# (:167-177 硬编码 true; have_glamor=false 因 M0 shm-only; have_decorate=false 因
# 不建 libdecor; T6 建 xserver 时由其安装的真实 pc 覆盖此文件)。
XWLR_PIN="24.1.13"
if [ ! -f "$HOST_EXT_PC/xwayland.pc" ] \
   || [ "$(sed -n 's/^Version: //p' "$HOST_EXT_PC/xwayland.pc")" != "$XWLR_PIN" ]; then
    cat > "$HOST_EXT_PC/xwayland.pc" << EOF
Name: Xwayland
Description: X Server for Wayland (T5 暂存复刻版, 依据 xwayland-$XWLR_PIN hw/xwayland/meson.build; T6 接管)
Version: $XWLR_PIN
xwayland=$HOST_EXT_USR/bin/Xwayland
have_glamor=false
have_glamor_api=false
have_eglstream=false
have_initfd=true
have_listenfd=true
have_verbose=true
have_terminate_delay=true
have_no_touch_pointer_emulation=true
have_force_xrandr_emulation=true
have_geometry=true
have_fullscreen=true
have_host_grab=true
have_decorate=false
have_enable_ei_portal=false
have_byteswappedclients=true
have_hidpi=true
EOF
    log "xwayland.pc ($XWLR_PIN 暂存复刻) → $HOST_EXT_PC"
fi

if [ -f "$OUT_LIB" ] && [ -f "$OUT_INC/wlr/backend.h" ]; then
    log "wlroots ($NATIVE_ARCH, $WLR_VER) 已就绪，跳过"
else
    # 构建目录带版本后缀 (旧 meson 缓存不可复用)
    # 踩坑: meson 1.10 移除了内建选项的连字符别名 (-Ddefault-library 报
    # "Unknown option"), 必须写下划线 -Ddefault_library 形式; 1.9 及以前两种都收
    # werror=false 是上游姿态: wlroots 项目默认 werror=true, 但其自身作为被
    # 依赖方时的 fallback (xwayland/meson.build default_options) 就传
    # werror=false。实测依据: OHOS bionic sysroot 的 __assert_fail 不带 noreturn
    # 属性 (glibc 带), xwm.c:306 的 assert 终止路径被 clang 判为缺 return,
    # -Werror 下直接失败; 关掉 -Werror 后该警告仍留在日志里可见。
    meson_host_build "$BUILD_DIR/wlroots_build_${WLR_VER}" "$WLR_SRC" \
        --prefix="$HOST_EXT_USR" --libdir=lib \
        -Dauto_features=disabled \
        -Dxwayland=enabled \
        -Ddefault_library=static \
        -Dexamples=false \
        -Dwerror=false
    ninja -C "$BUILD_DIR/wlroots_build_${WLR_VER}"
    ninja -C "$BUILD_DIR/wlroots_build_${WLR_VER}" install
fi

# ── 验收断言 ──
# 注: wlroots 无单头 wlr.h, 消费方按模块 include (wlr/backend.h 等)
[ -f "$OUT_LIB" ] || err "libwlroots-0.20.a 未产出"
for h in backend.h render/wlr_renderer.h render/pixman.h xwayland/xwayland.h; do
    [ -f "$OUT_INC/wlr/$h" ] || err "wlr 头未安装: $h"
done
[ -f "$HOST_EXT_PC/$WLR_PC_NAME.pc" ] || err "wlroots pc 未安装"
LLVM="$OHOS_SDK/native/llvm/bin"
# readelf -h 对静态归档逐成员输出 header, 只取第一个成员判定
m=$("$LLVM/llvm-readelf" -h "$OUT_LIB" | awk '/Machine:/{print $2; exit}')
want=AArch64
[ "$NATIVE_TARGET" = "x86_64-linux-ohos" ] && want=Advanced   # x86-64 输出 "Advanced Micro Devices X86-64"
[ "$m" = "$want" ] || err "架构侧别错误 (期望 $want): $OUT_LIB → $m"
# grep -q 命中即早退会让上游 nm 吃 SIGPIPE 非零退出, 在 pipefail 下整条管道
# 被判失败——命中也报"缺符号" (实测踩坑)。grep -c 消费全量输入, 无此问题。
for sym in wlr_headless_backend_create wlr_seat_create wlr_xwayland_create wlr_pixman_renderer_create; do
    [ "$("$LLVM/llvm-nm" "$OUT_LIB" | grep -c " T $sym\$")" -ge 1 ] || err "缺符号: $sym"
done

# 试链接: .a 的未定义符号必须能被 pc 依赖链 (wayland/xkbcommon/pixman/drm/xcb)
# 完整闭合——静态构建本身不链接, 侧别/路径错乱只有链接时暴露 (真实链接, 非
# unresolved 容忍模式); 产物只进 build/ 不发布
LINKTEST="$BUILD_DIR/wlroots_linktest_$NATIVE_ARCH"
echo 'int main(void){return 0;}' > "$BUILD_DIR/wlroots_linktest.c"
WLR_LINK_FLAGS=$(PKG_CONFIG_LIBDIR="$HOST_EXT_PC:$HOST_EXT_SHARE/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT_EXT_PC:$SYSROOT_EXT/usr/share/pkgconfig" \
    "$PKG_CONFIG_BIN" --libs "$WLR_PC_NAME") || err "wlroots pc --libs 解析失败"
"$LLVM/clang" --target="$NATIVE_TARGET" --sysroot="$SYSROOT" -fuse-ld=lld \
    "$BUILD_DIR/wlroots_linktest.c" $WLR_LINK_FLAGS -o "$LINKTEST" \
    || err "试链接失败 (flags: $WLR_LINK_FLAGS)"
[ -x "$LINKTEST" ] || err "试链接产物缺失"

log "wlroots OK ($NATIVE_ARCH, $WLR_VER): $OUT_LIB"
