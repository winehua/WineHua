# 共享环境变量 — 被所有子脚本 source
# 不要直接执行此文件

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export HOST_OS="${HOST_OS:-$(uname -s)}"

# 跨平台 sed -i (GNU: sed -i, BSD: sed -i '')
if [ "$HOST_OS" = "Darwin" ]; then
    sed_i() { sed -i '' "$@"; }
else
    sed_i() { sed -i "$@"; }
fi

# OHOS SDK
# macOS 下直接从 PATH 查找命令行工具目录，并推导相关路径。
if [ "$HOST_OS" = "Darwin" ] && [ -z "${TOOL_HOME:-}" ]; then
    old_ifs="$IFS"
    IFS=:
    for bin_dir in $PATH; do
        [ -n "$bin_dir" ] || bin_dir=.
        candidate_home="$(cd -P "$bin_dir/.." 2>/dev/null && pwd || true)"
        if { [ -e "$bin_dir/ohpm" ] || [ -e "$bin_dir/hvigorw" ]; } \
           && [ -d "$candidate_home/sdk/default/openharmony" ]; then
            export TOOL_HOME="$candidate_home"
            break
        fi
    done
    IFS="$old_ifs"
fi
if [ "$HOST_OS" = "Darwin" ] && [ -z "${OHOS_SDK:-}" ] \
   && [ -d "${TOOL_HOME:-}/sdk/default/openharmony" ]; then
    export OHOS_SDK="$TOOL_HOME/sdk/default/openharmony"
fi
if [ "$HOST_OS" = "Darwin" ] && [ -z "${OHOS_SDK:-}" ]; then
    echo "ERROR: 未找到 HarmonyOS command-line-tools。请将其 bin 目录加入 PATH，或设置 OHOS_SDK。" >&2
    return 1 2>/dev/null || exit 1
fi
export OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export PATH="$TOOL_HOME/bin:$TOOL_HOME/tool/node/bin:$PATH"

if [ "$HOST_OS" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
    HOMEBREW_BISON_BIN="$(brew --prefix bison 2>/dev/null || true)/bin"
    if [ -x "$HOMEBREW_BISON_BIN/bison" ]; then
        export PATH="$HOMEBREW_BISON_BIN:$PATH"
    fi
fi

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

# ── 设备上的 Wine 运行时根目录 (由 rawfile zip 解压) ──
WINE_DEVICE_ROOT="/data/storage/el2/base/files/wine"

# 源码路径
WINE_SRC="$ROOT/thirdparty/wine"
BOX64_SRC="$ROOT/thirdparty/box64"
DXVK_SRC="$ROOT/thirdparty/dxvk"

# 产物路径
BUILD_DIR="$ROOT/build"          # 源码构建中间产物
SYSROOT_EXT="$BUILD_DIR/sysroot-ext"  # 交叉编译扩展 (不污染 SDK)
STAGING_DIR="$BUILD_DIR/staging"   # 打包临时目录
DXVK_BUILD_ROOT="$BUILD_DIR/dxvk/legacy"

# sysroot-ext 目录结构
SYSROOT_EXT_INC="$SYSROOT_EXT/usr/include"
SYSROOT_EXT_LIB="$SYSROOT_EXT/usr/lib/x86_64-linux-ohos"
SYSROOT_EXT_PC="$SYSROOT_EXT/usr/lib/pkgconfig"
SYSROOT_EXT_SHARE="$SYSROOT_EXT/usr/share"

# Linux/WSL 保留原路径；macOS/HarmonyOS 使用项目内扫描器和当前工具链的 pkg-config。
if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
    export PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-$(command -v pkg-config || true)}"
    export WAYLAND_SCANNER="${WAYLAND_SCANNER:-$BUILD_DIR/host-tools/bin/wayland-scanner}"
    [ -n "${PKG_CONFIG_BIN:-}" ] || err "pkg-config not found in PATH; run: brew install pkg-config"
else
    export PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-/usr/bin/pkg-config}"
    export WAYLAND_SCANNER="${WAYLAND_SCANNER:-/usr/local/bin/wayland-scanner}"
fi

# HAP 项目
WINEHUA="$ROOT"

# Native 层 libs 目录
NATIVE_LIBS="$WINEHUA/entry/libs/$NATIVE_ARCH"

# 编译并行
if [ -z "${JOBS:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else
        JOBS=4
    fi
fi
export JOBS

# 生成 meson cross file (路径依赖 ROOT, 不能硬编码)
gen_cross_file() {
    local cross="$BUILD_DIR/ohos-x86_64-cross.txt"
    # pkg-config wrapper: --with-path 替换默认搜索路径 (宿主系统 /usr/lib/pkgconfig
    # 会混入 x11.pc 等 → 交叉构建误用宿主库探测)
    local pcwrap="$BUILD_DIR/pkg-config-cross.sh"
    cat > "$pcwrap" << PWEOF
#!/bin/sh
# PKG_CONFIG_LIBDIR 替换默认搜索路径 (--with-path 只是追加, 宿主 /usr/lib 仍混入)
export PKG_CONFIG_LIBDIR="$SYSROOT_EXT_PC:$SYSROOT_EXT/usr/lib/x86_64-linux-ohos/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
exec "$PKG_CONFIG_BIN" "\$@"
PWEOF
    chmod +x "$pcwrap"
    cat > "$cross" << XEOF
[binaries]
c = '$OHOS_SDK/native/llvm/bin/clang'
cpp = '$OHOS_SDK/native/llvm/bin/clang++'
ar = '$OHOS_SDK/native/llvm/bin/llvm-ar'
strip = '$OHOS_SDK/native/llvm/bin/llvm-strip'
pkg-config = '$pcwrap'
wayland-scanner = '$WAYLAND_SCANNER'
# 交叉装的 python 工具 (与架构无关, 宿主可直接执行) — meson 默认不搜 cross 前缀 bin
glib-mkenums = '$SYSROOT_EXT/usr/bin/glib-mkenums'
gdbus-codegen = '$SYSROOT_EXT/usr/bin/gdbus-codegen'

[built-in options]
c_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-I$SYSROOT_EXT_INC']
c_link_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$SYSROOT_EXT_LIB']
# pkgconfigdir = libdir/pkgconfig (x86_64-linux-ohos 子目录), 与 /usr/lib/pkgconfig 都要
pkg_config_path = ['$SYSROOT_EXT/usr/lib/pkgconfig', '$SYSROOT_EXT/usr/lib/x86_64-linux-ohos/pkgconfig', '$SYSROOT/usr/lib/pkgconfig']

[properties]
# 不设 sys_root: 编译器 --sysroot 已在 c_args/c_link_args 中，
# sysroot-ext 的 .pc 使用绝对路径，无需额外拼接。
# 强制宿主不能执行交叉产物 (build/host 同为 x86_64 linux 时 meson 会误判
# "能跑" → cc.run() 真的执行 OHOS ELF 失败; 与 HiSH deps/libglib 一致的做法),
# 使 meson.can_run_host_binaries() 返回 false, gnulib 检测自动走 else 分支。
needs_exe_wrapper = true

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
log()  { echo -e "\033[32m[BUILD]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[WARN]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*" >&2; exit 1; }

# ── 共享工具函数 ──
find_first_existing_dir() {
    local candidate=""
    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -d "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

resolve_first_executable() {
    local candidate=""
    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}
