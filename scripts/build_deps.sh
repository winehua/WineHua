#!/bin/bash
# build_deps.sh 鈥?缂栨帓鎵€鏈変氦鍙夌紪璇戜緷璧?(freetype 鈫?wayland 鈫?xkbcommon)
# 鎵€鏈変骇鐗╁畨瑁呭埌 build/sysroot-ext/锛屼笉姹℃煋 OHOS SDK
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 鏋勫缓妯℃嫙灞備氦鍙夌紪璇戜緷璧?(Wine鐢? x86_64-linux-ohos) 鈫?sysroot-ext ==="

# 鎸変緷璧栭摼椤哄簭鎵ц (妯℃嫙灞備緷璧? 濮嬬粓 x86_64-linux-ohos)
bash "$SCRIPT_DIR/build_freetype.sh"
bash "$SCRIPT_DIR/build_wayland.sh"
bash "$SCRIPT_DIR/build_xkbcommon.sh"
# XKB 閿洏甯冨眬鏁版嵁 (xkeyboard-config, Wine 閿洏椹卞姩渚濊禆, 鏋舵瀯鏃犲叧)
bash "$SCRIPT_DIR/build_xkbconfig.sh"

# Native compositor 渚濊禆 (wayland-server for HAP) 鍦?build.sh 涓寜鏋舵瀯鍗曠嫭璋冪敤:
#   bash scripts/build_native.sh

log "妯℃嫙灞備緷璧栧氨缁? $SYSROOT_EXT"
echo ""
find "$SYSROOT_EXT" -type f | sort
