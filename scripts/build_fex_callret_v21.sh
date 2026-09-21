#!/usr/bin/env bash
# Continuation build for the v21 "return target cache bypass" diagnostic FEX.
#
# This only rebuilds the already-configured isolated tree at
# build/fex-rwx-probe/ec, which carries the v13-v21 read-only probe plus the
# candidate callret patch. It never stages patches into build/fex-src and never
# touches the v13-v20 probe binaries.
#
# The previous attempt failed at link time because the generated
# link.txt invokes a bare "arm64ec-w64-mingw32-ar", which is not on the default
# container PATH. scripts/env.sh resolves LLVM_MINGW but does not export its bin
# directory, so do that explicitly here and verify it before building.
set -euo pipefail
cd "$(dirname "$0")/.."

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
# shellcheck disable=SC1091
source scripts/env.sh >/dev/null 2>&1
export PATH="$LLVM_MINGW/bin:$PATH"

probe_root="$ROOT/build/fex-rwx-probe"
probe_out="$ROOT/artifacts/fex-rwx-probe"
log_file="${V21_BUILD_LOG:-/tmp/v21-fex-rebuild.log}"

command -v arm64ec-w64-mingw32-ar
command -v arm64ec-w64-mingw32-clang++

# The candidate patch must already be applied; fail closed otherwise.
for marker in \
    "$probe_root/source/Source/Windows/ARM64EC/Module.cpp:FEX-CALLRET-CONTROL" \
    "$probe_root/source/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp:discard the cached host return target" \
    "$probe_root/source/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp:Candidate-only: ARM64EC returns"
do
    file="${marker%%:*}"
    needle="${marker#*:}"
    grep -q -- "$needle" "$file" || { echo "missing candidate marker in $file" >&2; exit 1; }
done

cmake --build "$probe_root/ec" --target arm64ecfex -j "${JOBS:-8}" > "$log_file" 2>&1
tail -5 "$log_file"

src_dll="$probe_root/ec/Bin/libarm64ecfex.dll"
test -f "$src_dll"

cp "$src_dll" "$probe_out/libarm64ecfex-callret-v21-unstripped.dll"
cp "$src_dll" "$probe_out/libarm64ecfex-callret-v21.dll"
"$LLVM_MINGW/bin/llvm-strip" "$probe_out/libarm64ecfex-callret-v21.dll"

python3 - <<'PY'
from pathlib import Path

stripped = Path("artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll")
data = stripped.read_bytes()
assert b"[FEX-CALLRET-CONTROL]" in data, "candidate marker missing after strip"
assert data[:2] == b"MZ", "not a PE image"
print("stripped size:", len(data), "bytes")
PY

sha256sum "$probe_out/libarm64ecfex-callret-v21.dll"
sha256sum "$probe_out/libarm64ecfex-callret-v21-unstripped.dll"
sha256sum "$probe_out/ntdll-v18.so"
