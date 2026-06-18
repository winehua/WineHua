# 共享环境变量 — 被所有子脚本 source
# 不要直接执行此文件

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# OHOS SDK
export OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export PATH="$TOOL_HOME/bin:$TOOL_HOME/tool/node/bin:$PATH"

CLANG="$OHOS_SDK/native/llvm/bin/clang"
SYSROOT="$OHOS_SDK/native/sysroot"
TARGET="x86_64-linux-ohos"

# 工具
HNPCLI="$OHOS_SDK/toolchains/hnpcli"

# 源码路径
WINE_SRC="$ROOT/thirdparty/wine"
BOX64_SRC="$ROOT/thirdparty/box64"

# 产物路径
BUILD_DIR="$ROOT/build"          # 源码构建中间产物
SYSROOT_EXT="$BUILD_DIR/sysroot-ext"  # 交叉编译扩展 (不污染 SDK)
STAGING_DIR="$ROOT/out/staging"  # HNP 打包临时目录
HNP_LAYOUT="$STAGING_DIR/opt/honwine"
OUT_DIR="$ROOT/out"              # 最终产出

# sysroot-ext 目录结构
SYSROOT_EXT_INC="$SYSROOT_EXT/usr/include"
SYSROOT_EXT_LIB="$SYSROOT_EXT/usr/lib/x86_64-linux-ohos"
SYSROOT_EXT_PC="$SYSROOT_EXT/usr/lib/pkgconfig"
SYSROOT_EXT_SHARE="$SYSROOT_EXT/usr/share"

# HAP 项目
HONWINE="$ROOT"

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
