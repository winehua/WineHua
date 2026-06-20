# 共享环境变量 — 被所有子脚本 source
# 不要直接执行此文件

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# OHOS SDK
export OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export PATH="$TOOL_HOME/bin:$TOOL_HOME/tool/node/bin:$PATH"

CLANG="$OHOS_SDK/native/llvm/bin/clang"
SYSROOT="$OHOS_SDK/native/sysroot"

# ── Native 层架构 (鸿蒙设备 CPU, HAP .so 的目标) ──
# arm64-v8a: 真机 (AArch64)
# x86_64:    模拟器 / x86_64 设备
NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"

# ── Wine 模拟层目标 (始终 x86_64, Wine 本身是 x86_64 ELF) ──
TARGET="x86_64-linux-ohos"

# 根据 NATIVE_ARCH 推导 Native 层 LLVM target / meson cpu
case "$NATIVE_ARCH" in
    arm64-v8a)
        NATIVE_TARGET="aarch64-linux-ohos"
        NATIVE_CPU_FAMILY="aarch64"
        NATIVE_CPU="aarch64"
        ;;
    x86_64)
        NATIVE_TARGET="x86_64-linux-ohos"
        NATIVE_CPU_FAMILY="x86_64"
        NATIVE_CPU="x86_64"
        ;;
    all)
        # 双架构模式: 仅在 package.sh 构建 HAP 时使用
        # NATIVE_TARGET/NATIVE_CPU_FAMILY 不适用
        NATIVE_TARGET=""
        NATIVE_CPU_FAMILY=""
        NATIVE_CPU=""
        ;;
    *)
        echo "ERROR: 不支持的 NATIVE_ARCH: $NATIVE_ARCH (可选: arm64-v8a, x86_64, all)"
        exit 1
        ;;
esac

# ── 设备类型 ──
# pc:  普通鸿蒙设备 (有 execve, 有 HNP)
# pad: 鸿蒙 Pad (fork-only, 无 execve, 无 HNP)
DEVICE_TYPE="${DEVICE_TYPE:-pc}"

# ── 设备上的 Wine 运行时根目录 ──
if [ "$DEVICE_TYPE" = "pad" ]; then
    WINE_DEVICE_ROOT="/data/storage/el2/base/files/wine"
else
    WINE_DEVICE_ROOT="/data/service/hnp/winehua.org/winehua_0.1.0/opt/winehua"
fi

# ── 传给 C++ 的 Pad 编译宏 ──
# CMakeLists.txt 根据此变量添加 -DPAD_MODE
if [ "$DEVICE_TYPE" = "pad" ]; then
    export PAD_CFLAGS="-DPAD_MODE"
else
    export PAD_CFLAGS=""
fi

# 工具
HNPCLI="$OHOS_SDK/toolchains/hnpcli"

# 源码路径
WINE_SRC="$ROOT/thirdparty/wine"
BOX64_SRC="$ROOT/thirdparty/box64"

# 产物路径
BUILD_DIR="$ROOT/build"          # 源码构建中间产物
SYSROOT_EXT="$BUILD_DIR/sysroot-ext"  # 交叉编译扩展 (不污染 SDK)
STAGING_DIR="$ROOT/out/staging"  # HNP 打包临时目录
HNP_LAYOUT="$STAGING_DIR/opt/winehua"
OUT_DIR="$ROOT/out"              # 最终产出

# sysroot-ext 目录结构
SYSROOT_EXT_INC="$SYSROOT_EXT/usr/include"
SYSROOT_EXT_LIB="$SYSROOT_EXT/usr/lib/x86_64-linux-ohos"
SYSROOT_EXT_PC="$SYSROOT_EXT/usr/lib/pkgconfig"
SYSROOT_EXT_SHARE="$SYSROOT_EXT/usr/share"

# HAP 项目
WINEHUA="$ROOT"

# Native 层 libs 目录
NATIVE_LIBS="$WINEHUA/entry/libs/$NATIVE_ARCH"

# 编译并行
JOBS=${JOBS:-$(nproc)}

# 生成 meson cross file (路径依赖 ROOT, 不能硬编码)
gen_cross_file() {
    local cross="$BUILD_DIR/ohos-x86_64-cross.txt"
    cat > "$cross" << XEOF
[binaries]
c = '$OHOS_SDK/native/llvm/bin/clang'
cpp = '$OHOS_SDK/native/llvm/bin/clang++'
ar = '$OHOS_SDK/native/llvm/bin/llvm-ar'
strip = '$OHOS_SDK/native/llvm/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
wayland-scanner = '/usr/local/bin/wayland-scanner'

[built-in options]
c_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-I$SYSROOT_EXT_INC']
c_link_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$SYSROOT_EXT_LIB']
pkg_config_path = ['$SYSROOT_EXT/usr/lib/pkgconfig', '$SYSROOT/usr/lib/pkgconfig']

[properties]
# 不设 sys_root: 编译器 --sysroot 已在 c_args/c_link_args 中，
# sysroot-ext 的 .pc 使用绝对路径，无需额外拼接。

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
XEOF
    echo "$cross"
}

# meson 构建: touch 源码避免 NFS clock skew
meson_build() {
    local build="$1" src="$2"
    shift 2
    local cross="$(gen_cross_file)"
    # 源码时间戳可能来自 NFS (比本地时钟快), touch 到本地时间
    find "$src" -type f -exec touch {} + 2>/dev/null || true
    mkdir -p "$build"
    meson setup "$build" "$src" --cross-file "$cross" "$@"
}

# 日志
log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*"; exit 1; }
