#!/bin/bash
# build_arm64_native.sh 鈥?鍏煎鎬у寘瑁呭櫒, 濮旀墭缁?build_native.sh
# 宸茬敱 build_native.sh 鏇夸唬, 淇濈暀姝ゆ枃浠剁敤浜庡悜鍚庡吋瀹?
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export NATIVE_ARCH=arm64-v8a
exec bash "$SCRIPT_DIR/build_native.sh"
