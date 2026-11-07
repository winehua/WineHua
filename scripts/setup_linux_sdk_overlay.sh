#!/bin/bash
# Reuse Windows OHOS SDK headers/libraries with native Linux LLVM tools.
# This is a local build workaround, not an official Linux OHOS toolchain.
set -euo pipefail
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <SDK root containing native/> <new overlay directory>" >&2
    exit 2
fi
sdk="$(realpath "$1")"
overlay="$(realpath -m "$2")"
[ "$(uname -s)" = Linux ] || { echo 'Linux is required' >&2; exit 1; }
[ -d "$sdk/native/sysroot" ] || { echo 'SDK sysroot missing' >&2; exit 1; }
[ ! -e "$overlay" ] || { echo "Overlay already exists: $overlay" >&2; exit 1; }
llvm_version="${LLVM_VERSION:-18}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
cc="$(command -v "clang-$llvm_version")"
cxx="$(command -v "clang++-$llvm_version")"
tools=(ld.lld llvm-ar llvm-ranlib llvm-nm llvm-strip llvm-readelf llvm-objcopy llvm-objdump)
for tool in "${tools[@]}"; do command -v "$tool-$llvm_version" >/dev/null; done
resources=("$sdk"/native/llvm/lib/clang/*)
[ "${#resources[@]}" = 1 ] && [ -d "${resources[0]}/include" ] || {
    echo 'Expected one SDK Clang resource directory' >&2; exit 1;
}
mkdir -p "$overlay/native/llvm/bin"
for item in "$sdk"/native/*; do
    [ "$(basename "$item")" = llvm ] || ln -s "$item" "$overlay/native/"
done
ln -s "$sdk/native/llvm/lib" "$overlay/native/llvm/lib"
ln -s "$sdk/native/llvm/include" "$overlay/native/llvm/include"
{
    printf 'SDK=%q\n' "$sdk"
    printf 'CC=%q\nCXX=%q\n' "$cc" "$cxx"
    printf 'RESOURCE_DIR=%q\n' "${resources[0]}"
    printf 'LLD=%q\n' "$(command -v "ld.lld-$llvm_version")"
} > "$overlay/native/llvm/bin/driver.conf"
cat > "$overlay/native/llvm/bin/clang" <<'WRAPPER'
#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/driver.conf"
compiler="$CC"
extra=()
if [[ "$(basename "$0")" = *++ ]]; then
    compiler="$CXX"
    extra+=(-nostdinc++ -isystem "$SDK/native/llvm/include/libcxx-ohos/include/c++/v1")
fi
target=
link=1
next_target=0
for arg in "$@"; do
    if [ "$next_target" = 1 ]; then target="$arg"; next_target=0; fi
    case "$arg" in
        --target=*|-target=*) target="${arg#*=}" ;;
        --target|-target) next_target=1 ;;
        -c|-E|-S|-fsyntax-only|-M|-MM|--version|-dumpversion|-dumpmachine) link=0 ;;
    esac
done
# Ninja/Meson can put --target and even -c entirely inside @response files.
# Inspect those tokens for SDK library lookup while passing the original
# arguments to Clang unchanged. Assignment propagates parser failures.
for arg in "$@"; do
    if [[ "$arg" = @* ]]; then
        inspection=$(python3 "$(dirname "$0")/linux_sdk_driver_args.py" "$@")
        target="${inspection%%$'\n'*}"
        link="${inspection##*$'\n'}"
        break
    fi
done
case "$target" in
    aarch64*-linux-ohos*) triple=aarch64-linux-ohos ;;
    x86_64*-linux-ohos*) triple=x86_64-linux-ohos ;;
    arm*-linux-ohos*) triple=arm-linux-ohos ;;
    *) triple= ;;
esac
if [ "$link" = 1 ] && [ -n "$triple" ]; then
    extra+=("--ld-path=$LLD" "-L$SDK/native/llvm/lib/$triple")
fi
exec "$compiler" -resource-dir="$RESOURCE_DIR" "${extra[@]}" "$@"
WRAPPER
chmod +x "$overlay/native/llvm/bin/clang"
cp "$script_dir/linux_sdk_driver_args.py" "$overlay/native/llvm/bin/"
ln -s clang "$overlay/native/llvm/bin/clang++"
for tool in "${tools[@]}"; do
    ln -s "$(command -v "$tool-$llvm_version")" "$overlay/native/llvm/bin/$tool"
done
printf 'OHOS_SDK=%s\n' "$overlay"
