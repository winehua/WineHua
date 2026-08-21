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

# check TOOL_HOME
if [ -z "$TOOL_HOME" ] || ! [ -d "$TOOL_HOME" ]; then
    echo "Please set the environment variable TOOL_HOME:"
    echo "    export TOOL_HOME=/path/to/command-line-tools"
    echo "You can download it from https://github.com/SwimmingTiger/command-line-tools/releases"
    exit 1
fi
export TOOL_HOME

set -x
# add hvigorw and binary-sign-tool to PATH
export PATH="$TOOL_HOME/bin:$TOOL_HOME/sdk/default/openharmony/toolchains/lib:$PATH"
{ set +x; } 2>/dev/null

if [ -z "$OHOS_SDK" ]; then
    set -x
    OHOS_SDK="$TOOL_HOME/sdk/default/openharmony"
    { set +x; } 2>/dev/null
fi

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
if [ -z "$LLVM_MINGW" ] || ! [ -d "$LLVM_MINGW" ]; then
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

# workaround with brew bash + cat and cp issue
# brew cat 会导致 cat: -: Broken pipe 报错，详见: https://atomgit.com/org/Harmonybrew/discussions/6
# brew cp 会导致 libffi.so.8 复制失败并得到损坏的文件，报错如下：
# cp: error deallocating '……/WineHua/entry/libs/arm64-v8a/libffi.so.8': Permission denied
# 并且报错后cp依然以状态码0退出，所以编译不会失败，但最终得到的hap会闪退。
CAT_PATH="$(command -v cat)"
CP_PATH="$(command -v cp)"
if [ "$CAT_PATH" = "$(brew --prefix)/bin/cat" ] || [ "$CP_PATH" = "$(brew --prefix)/bin/cp" ]; then
    echo "调整 PATH 让系统 cat 和 cp 命令优先级更高，避免 brew 的 cat 和 cp 命令导致编译失败"
    set -x
    mkdir -p "$BUILD_DIR/ohos-bin"
    ln -sf /usr/bin/cat "$BUILD_DIR/ohos-bin/"
    ln -sf /usr/bin/cp "$BUILD_DIR/ohos-bin/"
    export PATH="$BUILD_DIR/ohos-bin:$PATH"
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
rc=$?
{ set +x; } 2>/dev/null

if [ "$rc" -ne 0 ]; then
    echo "[ERROR] make 失败 (exit=$rc), 跳过 ELF 完整性检查"
    exit "$rc"
fi

# ELF 完整性检查 (make 完成后): 只校验最终生成的 unsigned HAP 内的 .so 未被截断。
# 历史教训: harmonybrew coreutils cp 写 HMDFS 时对部分 ELF 报 "error deallocating"
# 但退出码为 0 (非致命警告), 只写入前 10240 字节 → 截断的 libffi.so.8 静默进 HAP,
# 导致 app 启动时 loader mmap 越界 → SIGBUS。
check_elf_integrity() {
    echo "[CHECK] === ELF 完整性检查 (最终 HAP) ==="
    local arch="${NATIVE_ARCH:-arm64-v8a}"
    python3 - "$arch" <<'PYEOF'
import struct, os, sys, glob, zipfile
arch = sys.argv[1]

def elf_ok(d):
    if len(d) < 64 or d[:4] != b"\x7fELF":
        return None  # 非 ELF, 跳过
    e_phoff = struct.unpack_from("<Q", d, 0x20)[0]
    eps = struct.unpack_from("<H", d, 0x36)[0]
    np = struct.unpack_from("<H", d, 0x38)[0]
    mx = 0
    for i in range(np):
        o = e_phoff + i * eps
        if o + 56 > len(d):
            break
        if struct.unpack_from("<I", d, o)[0] == 1:  # PT_LOAD
            mx = max(mx, struct.unpack_from("<Q", d, o + 8)[0] + struct.unpack_from("<Q", d, o + 32)[0])
    return mx <= len(d)

bad = []
checked = 0
hap = "entry/build/default/outputs/default/entry-default-unsigned.hap"
if not os.path.isfile(hap):
    print(f"[CHECK] 未找到最终 HAP: {hap} (构建未产出, 视为失败)")
    sys.exit(1)
with zipfile.ZipFile(hap) as z:
    for n in z.namelist():
        if n.startswith(f"libs/{arch}/"):
            ok = elf_ok(z.read(n))
            if ok is None:
                continue
            checked += 1
            if not ok:
                bad.append(f"HAP:{n}")

print(f"[CHECK] 检查 {checked} 个 ELF (HAP libs/{arch})")
if bad:
    print("[CHECK] !! 发现截断/损坏的 ELF:")
    for b in bad:
        print("  !! " + b)
    sys.exit(1)
print("[CHECK] 全部完整 ✓")
sys.exit(0)
PYEOF
}

check_elf_integrity
exit $?
