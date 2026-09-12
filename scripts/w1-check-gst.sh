#!/bin/bash
# W1 调试：检查 winegstreamer 链接时 gstreamer 库有没有被判出来。
B=/data/src/winehua/build/wine-ohos-aarch64

echo "== 顶层 Makefile 里的 GSTREAMER_* =="
grep -n -E '^GSTREAMER_(LIBS|CFLAGS)' "$B/Makefile" | head -4

echo
echo "== winegstreamer 子 Makefile 里引用的 GSTREAMER_LIBS =="
grep -n 'GSTREAMER_LIBS' "$B/dlls/winegstreamer/Makefile" | head -4

echo
echo "== config.log 里 gstreamer 相关结论 =="
grep -n -i 'gstreamer' "$B/config.log" | tail -8

echo
echo "== sysroot-ext 里的 gstreamer .pc =="
ls /data/src/winehua/build/sysroot-ext/usr/lib/aarch64-linux-ohos/pkgconfig/ 2>/dev/null | grep -i gst | head -8

echo
echo "== 直接问 pkg-config（模拟 configure 的探测） =="
export PKG_CONFIG_PATH=/data/src/winehua/build/sysroot-ext/usr/lib/aarch64-linux-ohos/pkgconfig
pkg-config --exists gstreamer-1.0 && echo "gstreamer-1.0 exists" || echo "gstreamer-1.0 MISSING"
pkg-config --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0 2>&1 | head -3
pkg-config --cflags gstreamer-1.0 2>&1 | head -3

echo
echo "== config.log 里 pkg-config 那段 =="
grep -n -B3 -A6 'checking for GSTREAMER' "$B/config.log" 2>/dev/null | head -20
