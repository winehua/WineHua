#!/usr/bin/env bash
# 跨仓库手抄常量的成对性检查 (review 2026-10-01 P7: 三处契约是手抄副本, 无检查)
#
# 正确命令 + 正确目录 + 产物: 在仓库根执行 `bash scripts/check-contracts.sh`;
# 无产物, 退出码非零即有人改单边。用法: 改过下列任一文件后、合并前跑一次。
#
# 为什么必须检查 (失效后果分级):
#   - vtest WineHua 段版本不符: guest/宿主是**响**的 (-EPROTONOSUPPORT);
#   - present 页 magic/字段序不符: **静默全哑** (读不到 id ⇒ 一帧都不 present);
#   - display-fps 路径漂移: **判据静默失效** (判定读到旧/无文件, displayed 门失去意义)。
#
# 踩坑点 (SDK/版本耦合):
#   - thirdparty/mesa 与 thirdparty/virglrenderer 各自持有一份 vtest_protocol.h,
#     上游同步 (merge/rebase) 可能只动一份; 本脚本比对的是"WineHua 私有段"。
#   - 页结构体两份: wine 侧 win32u/winehua_present.c 与 mesa 的 virgl_vtest_socket.c;
#     字段名/顺序/类型任一不同即 ABI 破裂。
#   - display-fps 路径: graphics_broker.cpp 写的是 **Windows 字面量**
#     (C:\windows\temp\...), perf_utils.cpp 写的是 **Unix 字面量**
#     (/data/storage/el2/base/files/.wine/drive_c/windows/temp/...), 两者由
#     WINEPREFIX (wine_constants.h) 隐含绑定 —— 改前缀或改任一侧都要一起改。

set -eo pipefail

cd "$(dirname "$0")/.."

ROOT="$PWD"
fail=0
note() { printf '  %s\n' "$*"; }
bad()  { printf '\033[31m  ✗ %s\033[0m\n' "$*"; fail=1; }
ok()   { printf '\033[32m  ✓ %s\033[0m\n' "$*"; }

# 抽取 [起, 止) 之间的行, 去掉空行与注释, 便于比对
extract() {
  local file="$1" start="$2" end="$3"
  awk -v s="$start" -v e="$end" '
    index($0, s) { on = 1 }
    on && index($0, e) && !index($0, s) { exit }
    on { gsub(/^[ \t]+|[ \t]+$/, ""); if ($0 != "" && $0 !~ /^\//) print }
  ' "$file"
}

echo "== 1. vtest WineHua present 段: mesa ↔ virglrenderer =="
MESA_VTEST="thirdparty/mesa/src/virtio/vtest/vtest_protocol.h"
VKR_VTEST="thirdparty/virglrenderer/vtest/vtest_protocol.h"
for f in "$MESA_VTEST" "$VKR_VTEST"; do
  [ -f "$ROOT/$f" ] || { bad "缺少 $f"; }
done
if [ "$fail" -eq 0 ]; then
  a="$(extract "$ROOT/$MESA_VTEST" 'WineHua private extension' 'VCMD_WINEHUA_VK_PRESENT')"
  b="$(extract "$ROOT/$VKR_VTEST" 'WineHua private extension' 'VCMD_WINEHUA_VK_PRESENT')"
  if [ "$a" = "$b" ]; then ok "present 段逐行一致 ($(printf '%s\n' "$a" | wc -l) 行)"; else
    bad "present 段不一致 —— 两端必须同时改 (版本不符会被 guest 拒绝)"
    diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | head -20 || true
  fi
fi

echo "== 2. present 页结构: wine win32u ↔ mesa vtest socket =="
WINE_PAGE="thirdparty/wine/dlls/win32u/winehua_present.c"
MESA_PAGE="thirdparty/mesa/src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c"
# 只比字段声明 (顺序 + 类型 + 名字): 大括号风格/缩进不参与, 那是噪音
a="$(awk '/struct winehua_present_surface_page/{on=1} on{print} on && /};/{exit}' "$ROOT/$WINE_PAGE" \
     | grep -oE '(u?int[0-9]+_t|char) +[a-z_]+;' || true)"
b="$(awk '/struct winehua_present_surface_page/{on=1} on{print} on && /};/{exit}' "$ROOT/$MESA_PAGE" \
     | grep -oE '(u?int[0-9]+_t|char) +[a-z_]+;' || true)"
if [ "$a" = "$b" ]; then ok "页结构一致"; else
  bad "页结构不一致 —— 字段序/magic 不符会静默全哑 (读不到 id ⇒ 一帧不 present)"
  diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") || true
fi
for pair in "MAGIC 0x57535053" "VERSION 1"; do
  set -- $pair
  if ! grep -q "define WINEHUA_PRESENT_SURFACE_$1 $2" "$ROOT/$WINE_PAGE" "$ROOT/$MESA_PAGE"; then
    bad "WINEHUA_PRESENT_SURFACE_$1 不是 $2 (两侧都要)"
  fi
done

echo "== 3. display-fps 路径: graphics_broker (Windows 字面量) ↔ perf_utils (Unix 字面量) =="
BROKER="entry/src/main/cpp/graphics/graphics_broker.cpp"
PERF="entry/src/main/cpp/common/perf_utils.cpp"
win_path="$(grep -oE 'C:\\\\windows\\\\temp\\\\[A-Za-z0-9_.]+' "$ROOT/$BROKER" | head -1 || true)"
unix_path="$(grep -oE '/data/storage/el2/base/files/\.wine/drive_c/windows/temp/[A-Za-z0-9_.]+' "$ROOT/$PERF" | head -1 || true)"
name_win="${win_path##*\\\\}"
name_unix="${unix_path##*/}"
if [ -n "$name_win" ] && [ "$name_win" = "$name_unix" ]; then
  ok "文件名一致 ($name_win)"
else
  bad "display-fps 文件名不一致: broker=\"$win_path\" perf=\"$unix_path\" —— 判据会静默失效"
fi

echo "== 4. guest 侧判定字段: smoke 程序 ↔ 判定器 =="
GUEST="thirdparty/wine/programs/winehua_graphics_smoke/main.c"
CHECKS="automation/checks/__init__.py"
# guest 的 JSON 是在 C 字符串里拼的 ⇒ 文件里的字面量是 \"field\" (带反斜杠)
for field in expectedRoute presentedRoute declaredRoute presentedKey displayStallMs; do
  grep -q '\\"'$field'\\"' "$ROOT/$GUEST" || bad "guest 结果 JSON 缺字段 $field (判定器读它)"
done
# 判定器读了哪些 (提示级: 缺了不一定是错, 但要看得见)
for field in expectedRoute presentedRoute declaredRoute displayStallMs; do
  grep -q "metrics.get(\"$field\")" "$ROOT/$CHECKS" || note "(提示) 判定器未消费 $field"
done
[ "$fail" -eq 0 ] && ok "字段齐"

echo
if [ "$fail" -ne 0 ]; then
  echo "契约检查失败: 上面每一条都是成对改动，改完再跑本脚本。"
  exit 1
fi
echo "契约检查通过。"
