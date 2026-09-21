#!/usr/bin/env bash
# Identity check for the v21 return-target-cache-bypass FEX candidate.
set -euo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PY'
from pathlib import Path

path = Path("artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll")
data = path.read_bytes()
for marker in (b"[FEX-CALLRET-CONTROL]", b"[FEX-RWX-TRACK]", b"[FEX-RWX-CHAIN]", b"[FEX-RWX-UNPROTECT]"):
    print(f"{marker.decode():28s} occurrences={data.count(marker)}")
PY

echo "--- candidate vs v13-v20 read-only probe ---"
sha256sum artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll \
          artifacts/fex-rwx-probe/libarm64ecfex-rwx-probe.dll
echo "--- ntdll expectation (handoff section 1.2 v18) ---"
sha256sum artifacts/fex-rwx-probe/ntdll-v18.so
