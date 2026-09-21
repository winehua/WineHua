#!/usr/bin/env bash
# Show where the v21 control marker sits relative to the FEX startup output,
# and prove the shipped DLL really carries the marker string.
set -euo pipefail
cd "$(dirname "$0")/.."

src=build/fex-rwx-probe/source/Source/Windows/ARM64EC/Module.cpp
echo "--- Module.cpp occurrences ---"
grep -n 'FEX-CALLRET-CONTROL' "$src"
grep -n 'starting FEX based' "$src"

echo "--- Module.cpp context around the marker ---"
awk '/FEX-CALLRET-CONTROL/{for (i=NR-6; i<=NR+4; ++i) print i}' "$src" | sort -n -u | \
    while read -r line; do printf '%6d: %s\n' "$line" "$(sed -n "${line}p" "$src")"; done

echo "--- shipped candidate DLL marker bytes ---"
grep -a -o 'FEX-CALLRET-CONTROL' artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll | wc -l
grep -a -o 'starting FEX based' artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll | wc -l
echo "--- previously shipped v13-v20 probe DLL marker bytes ---"
grep -a -o 'FEX-CALLRET-CONTROL' artifacts/fex-rwx-probe/libarm64ecfex-rwx-probe.dll | wc -l

echo "--- objects vs sources timestamps ---"
ls -l --time-style=long-iso "$src"
ls -l --time-style=long-iso build/fex-rwx-probe/ec/Source/Windows/ARM64EC/CMakeFiles/arm64ecfex.dir/Module.cpp.obj
