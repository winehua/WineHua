#!/usr/bin/env bash
# Build the read-only Win32 window enumerator used to debug "processes alive but
# nothing on screen" in the OHOS Wine session.
set -euo pipefail
cd "$(dirname "$0")/.."

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
# shellcheck disable=SC1091
source scripts/env.sh >/dev/null 2>&1

out=artifacts/fex-rwx-probe/window-enum.exe
mkdir -p artifacts/fex-rwx-probe

"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" -O2 -Wall \
    -o "$out" tools/steam-rwx/window_enum.c

"$LLVM_MINGW/bin/llvm-strip" "$out" || true
ls -l "$out"
sha256sum "$out"
