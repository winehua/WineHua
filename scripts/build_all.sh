#!/bin/bash
# Legacy helper kept for low-level experiments.
# Prefer scripts/rebuild_harmony.ps1 or scripts/rebuild_harmony.sh for the
# supported MSYS2 / WSL build flow.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

case "${1:-}" in
    box64)  bash scripts/build_box64.sh ;;
    wine)   bash scripts/build_wine.sh ;;
    deps)   bash scripts/build_deps.sh ;;
    native) bash scripts/build_arm64_native.sh ;;
    full)
        bash scripts/build_box64.sh
        bash scripts/build_wine.sh
        ;;
    *)
        bash scripts/build_box64.sh
        ;;
esac

bash scripts/assemble.sh
bash scripts/package.sh hnp
bash scripts/package.sh hap
bash scripts/package.sh deploy
