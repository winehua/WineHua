# 共享环境变量 — 被所有子脚本 source
# 不要直接执行此文件

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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
STAGING_DIR="$ROOT/out/staging"  # HNP 打包临时目录
HNP_LAYOUT="$STAGING_DIR/opt/winebox"
OUT_DIR="$ROOT/out"              # 最终产出

# 补丁
PATCHES_DIR="$ROOT/patches"

# HAP 项目
HONWINE="$ROOT/HonWine"

# 编译并行
JOBS=${JOBS:-$(nproc)}

# 日志
log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*"; exit 1; }
