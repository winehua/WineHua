#!/bin/bash
# W1 / M2 准备：让 assemble.sh 能打包我们新编的 Proton-Wine-OHOS 候选。
#
# 做法：Wine 本体用**我们新编的** build/wine-ohos-aarch64；
# 第三方件（FEX / guest_gfx / guest_vulkan / host_vulkan / wine-mono）从产品工作树
# 链接过来复用（本轮不换这些层，正是方案要求的"只换 Wine Core"）。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 产品工作树：宿主上在 /home/liufeng/src/WineHua-arm64ec，容器里挂在 /data/prod。
# 允许用环境变量覆盖，避免"脚本在容器里跑却按宿主路径找 → 全部 skip"的坑。
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROD="${PROD:-/data/prod}"
[ -d "$PROD/build" ] || PROD=/home/liufeng/src/WineHua-arm64ec

cd "$ROOT/build" || exit 1

# 注意：**必须真实拷贝，不能用符号链接**。
# 构建在容器里跑，容器只挂了本工作树；指向产品工作树绝对路径的软链在容器内是断的
# （第一版用软链，assemble 就报 libarm64ecfex.dll / wowbox64.dll 未找到）。
for d in fex-ec fex-pe box64-pe guest_vulkan guest_gfx host_vulkan wine-mono dxvk vkd3d-proton; do
    if [ -e "$PROD/build/$d" ]; then
        rm -rf "$d"
        cp -a "$PROD/build/$d" "$d"
        echo "copy  $d <- $PROD/build/$d"
    else
        echo "skip  $d (产品工作树里没有)"
    fi
done

# assemble.sh 编译 smoke 时要 $DXVK_SRC/include（vulkan/vulkan.h 等）。
# 新工作树的 thirdparty/dxvk 子模块没初始化 → 从产品工作树拷一份源码目录过来。
if [ -d "$PROD/thirdparty/dxvk/include" ] && [ ! -d "$ROOT/thirdparty/dxvk/include" ]; then
    cp -a "$PROD/thirdparty/dxvk/." "$ROOT/thirdparty/dxvk/"
    echo "copy  thirdparty/dxvk <- $PROD/thirdparty/dxvk"
fi

# W1 移植：把 fork 自有的 programs/winehua_*（keep + 各类 smoke 探针）搬到
# Valve 树里（configure.ac 已注册对应 WINE_CONFIG_MAKEFILE）。assemble.sh 会打包它们。
FORK_PROGRAMS="winehua_audio_smoke winehua_d3d11_smoke winehua_dinput_probe
winehua_graphics_smoke winehua_keep winehua_vulkan_smoke"
forkprog_src="$ROOT/thirdparty/wine/programs"
forkprog_dst="$ROOT/thirdparty/wine-valve/programs"
for d in $FORK_PROGRAMS; do
    if [ -d "$forkprog_src/$d" ]; then
        rm -rf "$forkprog_dst/$d"
        cp -a "$forkprog_src/$d" "$forkprog_dst/$d"
        echo "copy  programs/$d <- fork"
    fi
done
[ -f "$forkprog_src/winehua_smoke_protocol.h" ] && \
    cp -a "$forkprog_src/winehua_smoke_protocol.h" "$forkprog_dst/"

# ── W1 移植：fork 独有的 OHOS 源码模块（台账 W-01/W-02/W-03/W-04/W-10）────
# 这些文件在 Valve 树里完全不存在（R0 审计：真正的 fork 独有文件 44 个）。
# 先做"文件落地"，注入点（unix/process.c、unix/file.c、unix/virtual.c 等）另轮逐个重放。
wp="$ROOT/thirdparty/wine"
wv="$ROOT/thirdparty/wine-valve"
copy_if_exists() {  # copy_if_exists <相对路径>
    if [ -e "$wp/$1" ]; then
        mkdir -p "$(dirname "$wv/$1")"
        cp -a "$wp/$1" "$wv/$1"
        echo "copy  $1 <- fork"
    fi
}

for f in \
    dlls/ntdll/unix/ohos_broker.c dlls/ntdll/unix/ohos_broker.h \
    dlls/ntdll/unix/ohos_file.c   dlls/ntdll/unix/ohos_file.h \
    dlls/ntdll/unix/ohos_virtual.c dlls/ntdll/unix/ohos_virtual.h \
    dlls/win32u/opengl_diag.c dlls/win32u/opengl_diag.h \
    dlls/winewayland.drv/opengl_diag.c dlls/winewayland.drv/opengl_diag.h \
    dlls/winewayland.drv/opengl_readback.c dlls/winewayland.drv/opengl_readback.h \
    dlls/winewayland.drv/wayland_surface_ohos.c dlls/winewayland.drv/wayland_surface_ohos.h \
    dlls/winewayland.drv/winehua-toplevel.xml \
    dlls/winebus.sys/bus_ohos.c \
    dlls/mciqtz32/mciqtz_waveout.c dlls/mciqtz32/mciqtz_waveout.h dlls/mciqtz32/minimp3.h \
    dlls/dnsapi/libresolv_musl.c \
    server/musl_compat.c ; do
    copy_if_exists "$f"
done

# wineohos.drv 是整目录
if [ -d "$wp/dlls/wineohos.drv" ]; then
    rm -rf "$wv/dlls/wineohos.drv"
    cp -a "$wp/dlls/wineohos.drv" "$wv/dlls/wineohos.drv"
    echo "copy  dlls/wineohos.drv <- fork (整目录)"
fi

# entry/libs/arm64-v8a: 我们的 build_wine.sh 已经把**新编的 wine unix .so** 放进去，
# 但 assemble 还需要 host 侧原生库（libvirglrenderer / libEGL / libGLESv2 / libc++_shared /
# kms_swrast_dri 等）。这些属于"本轮不动的层"，从产品工作树按 no-clobber 补齐：
# 已存在的（= 我们的新 wine 产物）绝不被覆盖。
if [ -d "$PROD/entry/libs/arm64-v8a" ]; then
    cp -an "$PROD/entry/libs/arm64-v8a/." "$ROOT/entry/libs/arm64-v8a/"
    echo "copy  entry/libs/arm64-v8a <- $PROD (no-clobber)"
fi

# assemble.sh 从 $BUILD_DIR/wine_server-$WINE_ARCH/libwineserver.so 取 wineserver。
# 必须用我们新编的（成套验收），不能用产品的那份。
mkdir -p wine_server-aarch64
cp -f "$ROOT/entry/libs/arm64-v8a/libwineserver.so" wine_server-aarch64/libwineserver.so
echo "wineserver(ours) -> $(ls -la wine_server-aarch64/libwineserver.so | awk '{print $5" bytes"}')"

echo
echo "== build/ 现状 =="
ls -la "$ROOT/build" | head -20
