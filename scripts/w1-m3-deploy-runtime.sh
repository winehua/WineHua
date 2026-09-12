#!/bin/bash
# W1 / M3 部署：把新编的 Proton-Wine-OHOS 运行时（wine-data.zip）推到设备并替换现有运行时。
#
# 前置：设备已连接（hdc list targets 能看到）。
# 用法：bash scripts/w1-m3-deploy-runtime.sh [hdc路径] [target]
#
# 做法（保守、可回退）：
#   1) 把 zip 推到 /data/local/tmp/
#   2) 备份设备上的 .winehua-runtime-manifest.json 与 files/wine 的清单
#   3) 用设备自带 unzip 解到 files/wine/（覆盖同名文件）
#   4) 把 manifest 的 payloadSha256 改成新包的哈希（app 据此判断"已是最新"，不会重解）
#   5) 打印核对信息
#
# 注意：本脚本不改 HAP，也不动 prefix（files/.wine）。要回退：
#   把 /data/local/tmp/ 下的备份 manifest 拷回，或重装上一版 HAP。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

HDC="${1:-/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe}"
TARGET="${2:-5KPBB25818203996}"
PKG="$ROOT/entry/src/main/resources/rawfile/wine-data.zip"
DEVICE_ROOT=/data/app/el2/100/base/app.hackeris.winehua/files

if [ ! -f "$PKG" ]; then echo "FATAL: $PKG 不存在（先跑 scripts/w1-m2-assemble.sh）" >&2; exit 2; fi

sha="$(sha256sum "$PKG" | awk '{print $1}')"
size="$(stat -c %s "$PKG")"
echo "包: $PKG"
echo "    sha256=$sha  size=$size"

run() { "$HDC" -t "$TARGET" shell "$1"; }

echo "== 设备检查 =="
if ! "$HDC" list targets 2>/dev/null | grep -q "$TARGET"; then
    echo "FATAL: 设备 $TARGET 未连接（hdc list targets 里没有）" >&2
    exit 3
fi

echo "== 1) 推送 zip =="
"$HDC" -t "$TARGET" file send "$PKG" /data/local/tmp/wine-data-w1.zip | tail -1

echo "== 2) 备份 =="
run "cp -f $DEVICE_ROOT/wine/.winehua-runtime-manifest.json /data/local/tmp/manifest.orig.json 2>/dev/null; \
     ls -ld $DEVICE_ROOT/wine > /data/local/tmp/wine.dir.list 2>&1; echo backup_done"

echo "== 3) 解包覆盖 =="
run "cd $DEVICE_ROOT/wine && unzip -o -q /data/local/tmp/wine-data-w1.zip && echo unzip_ok"

echo "== 4) 更新 manifest =="
run "printf '%s' '{\"schemaVersion\":1,\"payload\":\"wine-data.zip\",\"payloadSha256\":\"$sha\",\"smokeSuiteVersion\":\"w1-proton-wine-ohos\"}' > $DEVICE_ROOT/wine/.winehua-runtime-manifest.json && cat $DEVICE_ROOT/wine/.winehua-runtime-manifest.json"

echo "== 5) 核对 =="
run "ls $DEVICE_ROOT/wine/bin/aarch64-unix/ | head -5; echo '---'; ls $DEVICE_ROOT/wine/bin/aarch64-windows/ | wc -l; echo '---'; ls -la $DEVICE_ROOT/wine/bin/ | head -12"

echo
echo "完成。下一步跑 Gate W0（power shell 侧）："
echo "  hdc -t $TARGET shell \"aa force-stop app.hackeris.winehua\""
echo "  hdc -t $TARGET shell \"aa start -a EntryAbility -b app.hackeris.winehua\""
echo "  然后看 hilog 的 [WineChild] / [WineChild-stderr] 与设备上的 wine_stderr_*.log"
