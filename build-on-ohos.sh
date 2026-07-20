#!/usr/bin/env bash
set -x

export HOST_OS="HarmonyOS"
export PKG_CONFIG_BIN="$(command -v pkg-config)"
export CC=clang
export CXX=clang++

{ set +x; } 2>/dev/null

# 临时构建目录，必须位于 HMDFS 之外
BUILD_DIR="${BUILD_DIR:-/data/storage/el2/base/files/WineHua-build}"
TMPDIR="$BUILD_DIR/tmp"

# find uname-is-linux and ohos-sdk prefix from brew
BREW="$(command -v brew)"
if [ "$BREW" != "" ]; then
    LIBUNAME="$("$BREW" --prefix uname-is-linux)/lib/libuname.so"

    set -x
    # fix wine host tools missing libraries
    export LDFLAGS="-Wl,-rpath,$("$BREW" --prefix)/lib"
    { set +x; } 2>/dev/null

    if [ -z "$OHOS_SDK" ]; then
        set -x
        OHOS_SDK="$("$BREW" --prefix ohos-sdk)"
        { set +x; } 2>/dev/null
    fi
fi

# check ohos-sdk
if [ -z "$OHOS_SDK" ] || ! [ -d "$OHOS_SDK" ]; then
    echo "Please set the environment variable OHOS_SDK:"
    echo "    export OHOS_SDK=/path/to/ohos-sdk"
    echo "You can install ohos-sdk from Harmonybrew <https://harmonybrew.atomgit.com/>"
    echo "After finished the Harmonybrew setup, install ohos-sdk with this command:"
    echo "    brew install ohos-sdk"
    echo '    export OHOS_SDK="$(brew --prefix ohos-sdk)"'
    exit 1
fi
export OHOS_SDK

# check llvm-mingw
if [ -z "$LLVM_MINGW" ] || ! [ -d "$OHOS_SDK" ]; then
    echo "Please set the environment variable LLVM_MINGW:"
    echo "    export LLVM_MINGW=/path/to/llvm-mingw"
    echo "You can download it from https://github.com/SwimmingTiger/llvm-mingw/releases"
    exit 1
fi
export LLVM_MINGW

# check uname -s
if [ -f "$LIBUNAME" ]; then
    set -x
    export LD_PRELOAD="$LIBUNAME"
    { set +x; } 2>/dev/null
fi
UNAME_S="$(uname -s)"
if [ "$UNAME_S" != "Linux" ]; then
    echo "uname -s in your shell is $UNAME_S not Linux, some thirdparty projects will fail to build."
    echo "You can install uname-is-linux from Harmonybrew <https://harmonybrew.atomgit.com/> to fix this."
    echo "After finished the Harmonybrew setup, install uname-is-linux with this command:"
    echo "    brew install uname-is-linux"
    echo '    export LD_PRELOAD="$(brew --prefix uname-is-linux)/lib/libuname.so"'
    exit 1
fi

# We need the old cmake version from OHOS SDK.
# Harmonybrew cmake version is too high to some thirdparty projects.
set -x
export PATH="$OHOS_SDK/native/build-tools/cmake/bin:$OHOS_SDK/native/llvm/bin:$LLVM_MINGW/bin:$PATH"
{ set +x; } 2>/dev/null

# workaround with brew bash + cat issue
CAT="$(command -v cat)"
if [ "$CAT" = "$(brew --prefix)/bin/cat" ]; then
    echo "$CAT 会导致 cat: -: Broken pipe 报错，即将自动重命名"
    echo "详见: https://atomgit.com/org/Harmonybrew/discussions/6"
    set -x
    mv "$CAT" "$CAT.bak"
    { set +x; } 2>/dev/null
fi

cd "$(dirname "$0")"

if [ "$1" = "clean" ]; then
    set -x
    make clean
    rm -rf "$BUILD_DIR" ./build
    exit 0
fi

if [ "$(realpath ./build)" != "$BUILD_DIR" ]; then
    if [ -L ./build ]; then
        set -x
        rm ./build
        { set +x; } 2>/dev/null
    elif [ -e ./build ]; then
        DATE="$(date '+%Y%m%d-%H%M%S')"
        set -x
        mv ./build "./build-bak-$DATE"
        { set +x; } 2>/dev/null
    fi
    set -x
    mkdir -p "$BUILD_DIR"
    ln -s "$BUILD_DIR" ./build
    { set +x; } 2>/dev/null
fi

set -x
mkdir -p "$TMPDIR"
make NATIVE_ARCH=arm64-v8a "$@"
