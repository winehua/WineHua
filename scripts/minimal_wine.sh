#!/usr/bin/env bash
# 鍒涘缓鏈€灏忓寲 Wine 鍙戣鐗?(鐢ㄤ簬 Box64 on ARM64 OHOS)
#
# 鐢ㄦ硶: bash scripts/minimal_wine.sh
# 杈撳嚭: out/wine-minimal/

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FULL_WINE="$ROOT/out/wine"
MINIMAL="$ROOT/out/wine-minimal"

echo "==> Creating minimal Wine for Box64 on ARM64..."

rm -rf "$MINIMAL"
mkdir -p "$MINIMAL/bin" "$MINIMAL/lib/wine" "$MINIMAL/share/wine"

# 鈹€鈹€ Tier 1: 蹇呴』 (wineserver + ntdll) 鈹€鈹€
TIER1_BIN="wineserver"
TIER1_DLL="ntdll.dll ntdll.so"

# 鈹€鈹€ Tier 2: 鎺у埗鍙扮▼搴?鈹€鈹€
TIER2_BIN="cmd.exe wineconsole.exe"
TIER2_DLL="kernel32.dll kernelbase.dll ucrtbase.dll advapi32.dll"

# 鈹€鈹€ Tier 3: 绠€鍗?GUI (winecfg, notepad) 鈹€鈹€
TIER3_BIN="winecfg.exe notepad.exe"
TIER3_DLL="user32.dll gdi32.dll shell32.dll comctl32.dll comdlg32.dll
           shlwapi.dll ole32.dll oleaut32.dll rpcrt4.dll
           version.dll winspool.drv winmm.dll imm32.dll"

# 鈹€鈹€ Tier 4: 甯哥敤杩愯鏃?鈹€鈹€
TIER4_DLL="msvcrt.dll msvcp140.dll vcruntime140.dll concrt140.dll
           ws2_32.dll iphlpapi.dll dnsapi.dll crypt32.dll secur32.dll
           bcrypt.dll ncrypt.dll mpr.dll wininet.dll urlmon.dll"

echo ""

# 澶嶅埗鍑芥暟
copy_files() {
    local src_dir="$1" dst_dir="$2" list="$3" desc="$4"
    local ok=0 miss=0
    for f in $list; do
        local found=$(find "$src_dir" -name "$f" -type f 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            cp "$found" "$dst_dir/" 2>/dev/null && ok=$((ok+1)) || miss=$((miss+1))
        else
            miss=$((miss+1))
        fi
    done
    echo "  $desc: $ok copied, $miss missing"
}

copy_files "$FULL_WINE/bin" "$MINIMAL/bin" "$TIER1_BIN" "T1 bin"
copy_files "$FULL_WINE/lib/wine" "$MINIMAL/lib/wine" "$TIER1_DLL" "T1 lib"

copy_files "$FULL_WINE/bin" "$MINIMAL/bin" "$TIER2_BIN" "T2 bin"
copy_files "$FULL_WINE/lib/wine" "$MINIMAL/lib/wine" "$TIER2_DLL" "T2 lib"

copy_files "$FULL_WINE/bin" "$MINIMAL/bin" "$TIER3_BIN" "T3 bin"
copy_files "$FULL_WINE/lib/wine" "$MINIMAL/lib/wine" "$TIER3_DLL" "T3 lib"

copy_files "$FULL_WINE/lib/wine" "$MINIMAL/lib/wine" "$TIER4_DLL" "T4 lib"

# 澶嶅埗 nls 鏂囦欢 (Unicode 鏀寔)
if [ -d "$FULL_WINE/share/wine/nls" ]; then
    cp -r "$FULL_WINE/share/wine/nls" "$MINIMAL/share/wine/"
    echo "  nls: copied"
fi

echo ""
echo "============================================"
echo " Minimal Wine for Box64 on ARM64 OHOS"
echo "============================================"
echo " bin: $(ls $MINIMAL/bin | wc -l) files"
echo " lib: $(ls $MINIMAL/lib/wine | wc -l) files"
echo " size: $(du -sh $MINIMAL | cut -f1)"
echo ""
echo " Usage on ARM64 OHOS device:"
echo "   box64 -- ./bin/wineserver &"
echo "   box64 -- ./bin/wine cmd.exe"
echo ""
echo " Tiers:"
echo "   T1+T2: wine cmd          ~15 MB"
echo "   +T3:   wine notepad      ~80 MB"
echo "   +T4:   network apps      ~100 MB"

# 鎵撳寘
cd "$ROOT/out"
tar -czf wine-minimal-arm64-box64.tar.gz wine-minimal/
echo ""
echo " Archive: out/wine-minimal-arm64-box64.tar.gz ($(du -sh wine-minimal-arm64-box64.tar.gz | cut -f1))"
