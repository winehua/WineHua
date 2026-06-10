#!/usr/bin/env bash
# Wine for HarmonyOS — Build Script (Phase 1: x86_64)
#
# 用法:
#   bash scripts/build_wine_ohos.sh [--clean] [--with-patches]
#
# 前置条件:
#   1. OHOS SDK 已安装在 /apps/harmony/
#   2. Wine 源码已 clone 到 .temp/wine/
#   3. (首次) 需要先 build native Wine 工具链
#
# 输出: out/wineserver, out/ntdll.so 等
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_SRC="$ROOT/.temp/wine"
OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
OHOS_ARCH="${OHOS_ARCH:-x86_64}"

# OHOS 工具链
CLANG="$OHOS_SDK/native/llvm/bin/clang"
SYSROOT="$OHOS_SDK/native/sysroot"
TARGET="${OHOS_ARCH}-linux-ohos"
TOOLCHAIN_CMAKE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake"

# Wine 输出
OUT_DIR="$ROOT/out"
NATIVE_BUILD="$WINE_SRC/build-native"

# 处理参数
DO_CLEAN=0
APPLY_PATCHES=0
for arg in "$@"; do
    case "$arg" in
        --clean) DO_CLEAN=1 ;;
        --with-patches) APPLY_PATCHES=1 ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

echo "============================================"
echo " Wine for HarmonyOS — Build Script"
echo "============================================"
echo " OHOS SDK: $OHOS_SDK"
echo " Target:   $TARGET"
echo " Arch:     $OHOS_ARCH"
echo ""

# ================================================================
# Step 1: 检查前置条件
# ================================================================
check_prereqs() {
    echo "==> 检查前置条件..."

    for tool in "$CLANG" "$SYSROOT/usr/lib/$TARGET/libc.so"; do
        if [ ! -e "$tool" ]; then
            echo "ERROR: $tool 不存在"
            exit 1
        fi
    done
    echo "    OHOS SDK: OK"

    if [ ! -d "$WINE_SRC" ]; then
        echo "ERROR: Wine 源码不存在: $WINE_SRC"
        echo "       请先执行: git clone --depth=1 https://github.com/wine-mirror/wine.git $WINE_SRC"
        exit 1
    fi
    echo "    Wine 源码: OK"

    # 检查 native 工具
    if [ ! -f "$NATIVE_BUILD/tools/winebuild/winebuild" ]; then
        echo "    Native 工具未构建，正在编译..."
        build_native_tools
    fi
    echo ""
}

# ================================================================
# Step 2: 构建 Native Wine 工具链
# ================================================================
build_native_tools() {
    echo "==> 构建 Native Wine 工具..."
    mkdir -p "$NATIVE_BUILD"
    cd "$NATIVE_BUILD"

    # 用系统 gcc 最小化配置
    if [ ! -f Makefile ]; then
        "$WINE_SRC/configure" \
            --enable-win64 \
            --disable-tests \
            --without-x \
            --without-freetype \
            --without-alsa \
            --without-opengl \
            --without-vulkan
    fi

    make -j$(nproc)
    echo "    Native 工具构建完成"
    cd "$ROOT"
}

# ================================================================
# Step 3: 应用 HarmonyOS 适配补丁
# ================================================================
apply_wine_patches() {
    if [ "$APPLY_PATCHES" -eq 0 ]; then
        echo "==> 跳过补丁 (使用 --with-patches 启用)"
        return
    fi

    echo "==> 应用 HarmonyOS 适配补丁..."
    PATCH_SCRIPT="$ROOT/scripts/patches.sh"
    if [ -f "$PATCH_SCRIPT" ]; then
        WINE_SRC="$WINE_SRC" bash "$PATCH_SCRIPT"
    else
        echo "WARNING: patches.sh 不存在，跳过"
    fi
    echo ""
}

# ================================================================
# Step 4: 配置 OHOS 交叉编译
# ================================================================
configure_ohos() {
    echo "==> 配置 Wine for OHOS $TARGET..."

    WINE_OHOS_BUILD="$WINE_SRC/build-ohos"
    rm -rf "$WINE_OHOS_BUILD"
    mkdir -p "$WINE_OHOS_BUILD"
    cd "$WINE_OHOS_BUILD"

    # 关键: 用系统 gcc 处理 PE 部分, OHOS clang 处理 Unix 部分
    # Wine 的 configure 使用 CC 作为 Unix 编译器
    CC="gcc" \
    CFLAGS="-D__MUSL__ -D_GNU_SOURCE" \
    "$WINE_SRC/configure" \
        --host="$TARGET" \
        --with-wine-tools="$NATIVE_BUILD" \
        --with-mingw=gcc \
        --disable-tests \
        --without-x \
        --without-freetype \
        --without-alsa \
        --without-opengl \
        --without-vulkan \
        --without-gstreamer \
        --without-pulse \
        --without-oss \
        --without-cups \
        --without-fontconfig \
        --without-dbus \
        --without-udev

    echo "    Configure 完成: $WINE_OHOS_BUILD"
    echo ""
}

# ================================================================
# Step 5: 编译 wineserver (第一步验证目标)
# ================================================================
build_wineserver() {
    echo "==> 编译 wineserver..."

    cd "$WINE_SRC/build-ohos"
    make -j$(nproc) server/wineserver 2>&1 | tail -20

    if [ -f server/wineserver ]; then
        mkdir -p "$OUT_DIR"
        cp server/wineserver "$OUT_DIR/wineserver"
        echo "    wineserver 编译成功 → $OUT_DIR/wineserver"
        file "$OUT_DIR/wineserver"
    else
        echo "    wineserver 编译失败，请查看错误日志"
        exit 1
    fi
}

# ================================================================
# 主流程
# ================================================================
main() {
    check_prereqs
    apply_wine_patches
    configure_ohos

    # 阶段性编译
    build_wineserver

    echo ""
    echo "============================================"
    echo " Phase 1 (x86_64) 第一阶段完成"
    echo " 下一步: 编译 ntdll.so, 运行 wineconsole 测试"
    echo "============================================"
}

main
