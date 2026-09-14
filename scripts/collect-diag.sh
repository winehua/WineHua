#!/bin/bash
#
# collect-diag.sh — 一键采集设备端诊断现场
#
# 排查设备问题时把"该看什么"固化下来, 免去逐个回忆日志路径与 hdc 命令。
#
# 用法:
#     scripts/collect-diag.sh                          # 自动选设备
#     scripts/collect-diag.sh -t 192.168.1.6:33363     # 指定设备
#     scripts/collect-diag.sh --hilog-lines 20000
#     scripts/collect-diag.sh -o /tmp/mydiag
#
# 产出 (默认 build/diag-logs/<时间戳>/):
#     hilog.txt           应用日志 — 含 WineChild-stderr 转发的 wine stderr
#     processes.txt       wine / box64 进程列表
#     threads.txt         各进程: 主线程 comm/state + Threads 计数
#     devices.txt         设备与应用版本
#     sandbox-*.log       沙箱内日志文件 (见下方能力边界)
#     SUMMARY.txt         本次采集项的成功/失败一览
#
# 能力边界 (2026-09 实测, HarmonyOS tablet 192.168.1.6):
#   应用沙箱 /data/app/el2/100/base/<bundle>/{temp,cache,files} 对 shell 与
#   `hdc file recv` 都是 Permission denied — 文件类日志采不到, 脚本会标注
#   而不是静默跳过 (run_regression.py 的 save_device_file 就是不检查返回值
#   的写法, 失败时归档目录里干脆没有这些文件, 排查时容易误判"没有日志")。
#   缓解: WineChild-stderr 是双写 (hilog + 文件), 小量诊断输出仍能从
#   hilog.txt 拿到; 大流量 (WINEDEBUG=+relay) 需应用内导出 (main-ui 分支的
#   LogExportService 是现成参考)。
#
# 退出码: 0 = 至少采集到 hilog; 1 = 设备不可达或采集全失败

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUNDLE="app.hackeris.winehua"
SANDBOX="/data/app/el2/100/base/${BUNDLE}"
DEVICE="${WINEHUA_DEVICE:-}"
OUT_DIR=""
HILOG_LINES=8000

while [ $# -gt 0 ]; do
    case "$1" in
        -t) DEVICE="$2"; shift 2 ;;
        -o) OUT_DIR="$2"; shift 2 ;;
        --hilog-lines) HILOG_LINES="$2"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$DEVICE" ]; then
    DEVICE="$(hdc list targets 2>/dev/null | grep -vE '^\s*$|\[Empty\]' | head -1 | tr -d '[:space:]')"
    [ -n "$DEVICE" ] || { echo "错误: 无可用设备, 用 -t 指定" >&2; exit 1; }
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
[ -n "$OUT_DIR" ] || OUT_DIR="$ROOT/build/diag-logs/$STAMP"
mkdir -p "$OUT_DIR"

H="hdc -t $DEVICE"
SUMMARY="$OUT_DIR/SUMMARY.txt"
: > "$SUMMARY"
record() { printf '%-28s %s\n' "$1" "$2" >> "$SUMMARY"; }

echo "=== 采集诊断现场 ==="
echo "设备: $DEVICE"
echo "输出: $OUT_DIR"
echo ""

if ! $H shell "echo ok" >/dev/null 2>&1; then
    echo "错误: 设备 $DEVICE 不可达" >&2
    exit 1
fi

# --- 1. hilog (含 WineChild-stderr 转发的 wine 输出) ----------------------
if $H shell "hilog -z $HILOG_LINES -t app" > "$OUT_DIR/hilog.txt" 2>/dev/null \
   && [ -s "$OUT_DIR/hilog.txt" ]; then
    record "hilog.txt" "OK ($(wc -l < "$OUT_DIR/hilog.txt" | tr -d ' ') 行)"
else
    record "hilog.txt" "FAIL"
fi

# --- 2. 进程列表 ----------------------------------------------------------
if $H shell "ps -ef" 2>/dev/null | grep -iE 'winehua|box64|wine' \
   > "$OUT_DIR/processes.txt" && [ -s "$OUT_DIR/processes.txt" ]; then
    record "processes.txt" "OK ($(wc -l < "$OUT_DIR/processes.txt" | tr -d ' ') 行)"
else
    record "processes.txt" "FAIL (无匹配进程?)"
fi

# --- 3. 进程状态快照 ------------------------------------------------------
# 线程级枚举在当前设备/权限下**不可得** (2026-09 实测, HarmonyOS tablet):
#   /proc/<pid>/task 目录不可列 (ls: Permission denied; 通配符不展开),
#   ps -efT -p <pid> 只输出主线程, hidumper -p <pid> 无输出。
#   (同一台设备早期曾能列出全部线程, 权限状态随环境变化 — 不要假设它可用。)
# 能拿到: 进程列表 + 每进程主线程 comm/state + Threads 计数。
# 卡死定位因此依赖 hilog 心跳 (MW-RNDR / WL-STAT / Input-DROP) 与
# winehua.diag_env 注入的诊断输出, 而不是 /proc 线程遍历。
$H shell 'for p in $(ps -ef | grep -iE "winehua|box64" | grep -v grep | sed "s/  */ /g" | cut -d" " -f2); do
    echo "== pid=$p =="
    echo "  comm=$(cat /proc/$p/task/$p/comm 2>/dev/null) state=$(cut -d" " -f3 /proc/$p/task/$p/stat 2>/dev/null < /dev/null)"
    echo "  $(grep -i threads /proc/$p/status 2>/dev/null)"
done' > "$OUT_DIR/threads.txt" 2>/dev/null || true
if [ -s "$OUT_DIR/threads.txt" ]; then
    record "threads.txt" "OK ($(grep -c '^== ' "$OUT_DIR/threads.txt" | tr -d ' ') 进程, 含线程计数)"
else
    record "threads.txt" "FAIL"
fi

# --- 4. 设备与应用版本 ----------------------------------------------------
{
    echo "target: $DEVICE"
    echo "devicetype: $($H shell 'param get const.product.devicetype' 2>/dev/null | tr -d '\r')"
    echo "model: $($H shell 'param get const.product.model' 2>/dev/null | tr -d '\r')"
    echo "os: $($H shell 'param get const.ohos.apiversion' 2>/dev/null | tr -d '\r')"
    echo "app: $($H shell "bm dump -n $BUNDLE" 2>/dev/null | grep -iE 'versionName|versionCode' | head -4)"
} > "$OUT_DIR/devices.txt" 2>&1
record "devices.txt" "OK"

# --- 5. 沙箱日志文件 (取决于设备权限, 见文件头"能力边界") -----------------
fetch_sandbox() {
    local rel="$1" name="$2"
    if $H file recv "$SANDBOX/$rel" "$OUT_DIR/$name" >/dev/null 2>&1 \
       && [ -s "$OUT_DIR/$name" ]; then
        record "$name" "OK ($(wc -c < "$OUT_DIR/$name" | tr -d ' ') bytes)"
    else
        rm -f "$OUT_DIR/$name"
        record "$name" "SKIP (沙箱不可读, 见脚本头)"
    fi
}
TODAY="$(date +%Y%m%d)"
fetch_sandbox "temp/wine_stderr_${TODAY}.log"        "sandbox-wine-stderr.log"
fetch_sandbox "cache/winehua_virgl_host.log"         "sandbox-virgl-host.log"
fetch_sandbox "temp/winehua_vtest_frontbuffer.log"   "sandbox-vtest-frontbuffer.log"
fetch_sandbox "files/.wine/drive_c/windows/temp/winehua_display_fps.txt" "sandbox-display-fps.txt"

# --- 汇总 -----------------------------------------------------------------
echo ""
cat "$SUMMARY"
echo ""
sandbox_skipped="$(grep -c 'SKIP' "$SUMMARY" || true)"
if [ "${sandbox_skipped:-0}" -gt 0 ]; then
    echo "注意: ${sandbox_skipped} 项沙箱日志未采集 (设备权限限制)。"
    echo "      小量诊断输出仍可从 hilog.txt 获取 (WineChild-stderr 双写)。"
fi
echo "归档: $OUT_DIR"
