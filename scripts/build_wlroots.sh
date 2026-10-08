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

# ── host 侧 EGL / GLESv2 pc (M2-T4: wlroots gles2 渲染器) ──
# OHOS SDK sysroot 只装 .so 不装 .pc, 而 wlroots gles2 依赖 egl/glesv2 两个
# pkg-config 依赖项。这里按 SDK sysroot 实测路径生成(与 xwayland.pc 同款
# 就地生成法); 路径按 NATIVE_TARGET 取, 侧别不会串 (host-ext 目录本身就是
# 按 NATIVE_ARCH 隔离的)。
# 注: SDK 的 libEGL.so 是「桥接库」(真实现在 libEGL_inner 之类), 链接期
# 只需要 -lEGL/-lGLESv2 能被 --sysroot 找到 —— 与 entry 的 CMake 构建同源。
SDK_GL_LIBDIR="$SYSROOT/usr/lib/$NATIVE_TARGET"
[ -d "$SDK_GL_LIBDIR" ] || err "SDK 图形库目录不存在: $SDK_GL_LIBDIR"
cat > "$HOST_EXT_PC/egl.pc" << EOF
Name: egl
Description: EGL (OHOS SDK sysroot, host 侧)
Version: 1.5
Libs: -L$SDK_GL_LIBDIR -lEGL
Cflags: -I$SYSROOT/usr/include
EOF
cat > "$HOST_EXT_PC/glesv2.pc" << EOF
Name: glesv2
Description: OpenGL ES 2/3 (OHOS SDK sysroot, host 侧)
Version: 3.2
Libs: -L$SDK_GL_LIBDIR -lGLESv2
Cflags: -I$SYSROOT/usr/include
EOF
log "egl.pc / glesv2.pc → $HOST_EXT_PC (SDK $NATIVE_TARGET)"

# OHOS NCP 启动补丁 (out-of-tree, submodule 钉 tag 零本地提交, 同 xserver 惯例):
# fork/exec 替换为 NCP spawn 钩子 + 导出 argv 哨兵构建器。守卫 = 补丁签名,
# 只看产物存在会吃掉补丁改动 (wayland 头/xwayland 补丁同款教训)。
WLR_PATCH="$SCRIPT_DIR/patches/wlroots-ohos-ncp-spawn.patch"
# shm fchmod 容忍补丁 (M1-T1): OHOS 沙箱对 shm_open 文件 fchmod(0) 返回
# EACCES (SELinux setattr 限制, 2026-09-29 实测 errno=13), 该步骤是防护性
# 强化非正确性前提 → best-effort 降级。守卫签名 = 多份补丁串联哈希。
WLR_SHM_PATCH="$SCRIPT_DIR/patches/wlroots-ohos-shm-fchmod-tolerant.patch"
# gles2 + EGLImage 导入补丁 (M2-T4): OHOS 无 DRM/GBM, 上游 render/meson.build
# 把 gbm 依赖设为 required:'gles2' in renderers ⇒ 无 gbm.pc 时 gles2 整个不
# 可用 (而 gles2 渲染器本体不用 GBM, 只有 render/egl.c 的平台选择用)。同一
# 补丁加通用导入钩子: 无 DMA-BUF 的 wlr_buffer (OHOS NativeBuffer) 由宿主侧
# 注册的导入器接管成 EGLImage, 渲染目标与采样纹理共用同一 GL 对象。
WLR_GLES2_PATCH="$SCRIPT_DIR/patches/wlroots-ohos-gles2-egl-import.patch"
# xwm 子窗几何跟踪补丁 (D23): wine 虚拟桌面的应用窗都是桌面顶层的 X 子窗口,
# xwm 原本只在 root 选 SubstructureNotify (只覆盖顶层), 子窗几何无人跟踪 ⇒
# host 零拷贝面锚定查不到 (TryAttach 死区, dxvk/GL 44 帧后停摆)。补丁给托管
# 窗加选 SUBSTRUCTURE_NOTIFY, 维护子窗几何表 (相对坐标, 查询时算绝对), 暴露
# wlr_xwayland_query_child_geometry() 供 ohos_output 锚定回退。不建 xsurface
# (不污染 client list / restack 语义)。
WLR_XWM_PATCH="$SCRIPT_DIR/patches/wlroots-ohos-xwm-child-geometry.patch"
# xwm stack-only restack 补丁 (D37): wine SetWindowPos 的 Z 序变更走
# XReconfigureWMWindow (window.c:1514) → 顶层 ConfigureRequest, 上游 xwm 对
# 无几何位的请求 (geo_mask==0) 直接丢弃 (TODO) ⇒ HWND_TOP/BOTTOM 带
# SWP_NOACTIVATE 的 restack 全部蒸发, win32 内序 (D33 修复) 与合成器视觉序
# 脱节。补丁对 Above/Below 的 stack-only 请求调 wlr_xwayland_surface_restack
# (TopIf/BottomIf/Opposite 沿 restack() 的 abort 语义跳过, wine 不发),
# 并发新事件 request_restack{surface,sibling,mode} 供 ohos_output 把
# g_clients 链表序 + scene 节点序跟着改 (事件在 X 侧 restack 之后发, 两侧一致)。
WLR_RESTACK_PATCH="$SCRIPT_DIR/patches/wlroots-ohos-xwm-restack-event.patch"
# scene damage 台账插桩补丁 (D25-B): 画布冻结坏变体「seq 推进而
# needsFrame=0」的抓捕仪器 —— 三个记账点 (surface commit 到达 /
# scene_output_damage 入账 / output commit 消费) 对账定位断跳。旗标文件
# 与宿主 DiagFlagFile 同一个 (.wine/drive_c/scene-damage-diag, 1s 缓存),
# 关闭近零开销, 可常驻。
WLR_DIAG_PATCH="$SCRIPT_DIR/patches/wlroots-scene-damage-diag.patch"
WLR_PATCH_SIG=$(cat "$WLR_PATCH" "$WLR_SHM_PATCH" "$WLR_GLES2_PATCH" "$WLR_XWM_PATCH" "$WLR_RESTACK_PATCH" "$WLR_DIAG_PATCH" | sha256sum | cut -d' ' -f1)

if [ -f "$OUT_LIB" ] && [ -f "$OUT_INC/wlr/backend.h" ] \
   && [ "$(cat "$HOST_EXT_LIB/.wlroots_patch_sig" 2>/dev/null)" = "$WLR_PATCH_SIG" ]; then
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
    # 补丁应用: 本分支入口 = 补丁签名与守卫不符 (补丁变更或首次构建)。
    # 先复位子模块工作区到基线再 apply——sentinel 命中会把「树里是旧补丁」
    # 误判为「已应用」, 新 hunks 静默丢失 (与 build_xwayland.sh 同款教训)。
    git -C "$WLR_SRC" checkout -- xwayland include/xwayland/xwm.h include/wlr/xwayland/server.h \
        include/wlr/xwayland/xwayland.h util/shm.c \
        meson.build render/meson.build include/wlr/config.h.in \
        include/wlr/render/egl.h include/render/egl.h include/render/gles2.h \
        render/egl.c render/gles2/renderer.c render/gles2/texture.c \
        types/scene/wlr_scene.c types/scene/surface.c
    rm -f "$WLR_SRC/xwayland/ohos_spawn.c"
    git -C "$WLR_SRC" apply --check "$WLR_PATCH" "$WLR_SHM_PATCH" "$WLR_GLES2_PATCH" "$WLR_XWM_PATCH" "$WLR_RESTACK_PATCH" "$WLR_DIAG_PATCH" \
        || err "wlroots 补丁无法应用 (submodule 工作区与补丁基线不符)"
    git -C "$WLR_SRC" apply "$WLR_PATCH" "$WLR_SHM_PATCH" "$WLR_GLES2_PATCH" "$WLR_XWM_PATCH" "$WLR_RESTACK_PATCH" "$WLR_DIAG_PATCH"
    # 源级断言 (D25-B): sentinel 命中会把「树里是旧补丁」误判为已应用,
    # 新 hunks 静默丢失 —— apply 后核对插桩代码里的旗标路径串 (存在性,
    # 非精确计数: 注释+代码可多处命中, 实测 wlr_scene.c=2 surface.c=1)
    [ "$(grep -c 'scene-damage-diag' "$WLR_SRC/types/scene/wlr_scene.c")" -ge 1 ] \
        && [ "$(grep -c 'scene-damage-diag' "$WLR_SRC/types/scene/surface.c")" -ge 1 ] \
        || err "scene damage diag 插桩缺失 (D25-B 补丁未生效)"
    meson_host_build "$BUILD_DIR/wlroots_build_${WLR_VER}" "$WLR_SRC" \
        --prefix="$HOST_EXT_USR" --libdir=lib \
        -Dauto_features=disabled \
        -Dxwayland=enabled \
        -Drenderers=gles2 \
        -Ddefault_library=static \
        -Dexamples=false \
        -Dwerror=false
    ninja -C "$BUILD_DIR/wlroots_build_${WLR_VER}"
    ninja -C "$BUILD_DIR/wlroots_build_${WLR_VER}" install
    echo "$WLR_PATCH_SIG" > "$HOST_EXT_LIB/.wlroots_patch_sig"
fi

# ── 验收断言 ──
# 注: wlroots 无单头 wlr.h, 消费方按模块 include (wlr/backend.h 等)
[ -f "$OUT_LIB" ] || err "libwlroots-0.20.a 未产出"
for h in backend.h render/wlr_renderer.h render/pixman.h render/gles2.h \
         render/egl.h xwayland/xwayland.h; do
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
for sym in wlr_headless_backend_create wlr_seat_create wlr_xwayland_create \
           wlr_pixman_renderer_create wlr_gles2_renderer_create \
           wlr_egl_create_with_context wlr_egl_set_buffer_image_importer \
           wlr_egl_set_buffer_image_importer_formats; do
    [ "$("$LLVM/llvm-nm" "$OUT_LIB" | grep -c " T $sym\$")" -ge 1 ] || err "缺符号: $sym"
done
# 源级补丁断言: 客户端扩展查询不可用时的降级路径必须真在源里。产物侧看不出来
# —— 丢了这段 hunk, gles2 渲染器在 OHOS 上整条不启动 (实测: egl_create 里
# EGL_EXT_client_extensions 缺失 ⇒ 回退 pixman), 只有真机日志能看出。
[ "$(grep -c "egl_create_with_client_exts" "$WLR_SRC/render/egl.c")" -ge 2 ] \
    || err "render/egl.c 缺客户端扩展降级路径 (gles2-egl-import 补丁 hunk 丢失?)"
[ "$(grep -c "request_restack" "$WLR_SRC/xwayland/xwm.c")" -ge 2 ] \
    || err "xwm.c 缺 stack-only restack 通道 (restack-event 补丁 hunk 丢失?)"

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
