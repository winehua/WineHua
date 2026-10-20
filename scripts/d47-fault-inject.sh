#!/usr/bin/env bash
# D47 [AUTO-RETRY] 故障注入验证 —— 验证「首启失败自动重试」链（F1 止血 1d83b64）
# 真实生效，全程零产品代码改动。
#
# 注入方式（2026-10-11 三次尝试后定稿）：**首启 wineboot 中途 `aa force-stop`
# 强制打断 → 立即重拉新进程**（模拟「首启被中断/崩溃后用户重启应用」场景，
# 即 D47 现场轨迹；半成品前缀上重启的失败面与「4 连崩」同族）。重拉 = 新进程
# （engineEverReady=false，重试门槛满足）。之后必须观察到：
#   state:failed → [AUTO-RETRY] cold-start engine fail 行 → 自动 restartEngine
#   → 新 boot 达 state:ready。若重拉 boot 干净走 ready（未触发）→ 非零退出，
#   重跑本脚本或换中断点再试（注入以触发为准, 脚本不掩盖）。
# 为何不用外部 signal 杀 wine 进程（实测记录）：hdc shell (uid 2000) 对 app uid
# 子进程 kill -9 报 EPERM（DAC 越 uid），此路不通；aa force-stop 是 shell 域
# 唯一可用的外力。
# 判定纪律 (R3 复核): 必须认 hilog `[AUTO-RETRY]` 行 —— 其它自动恢复路径
# (winehua.program ensureEngineReady) 会抢跑同款 restartEngine, 只看「恢复达
# ready」会误判。
# 实测记录（2026-10-11，4 轮未触发；用户随后决定暂缓追该崩溃排查，脚本
# 保留作未来复现/再注入时用，未取得 PASS 记录）：外部 kill 两路已排除
# （wineboot loader exit=9 被当完成 / wineserver EPERM）；+18s 与 +42s 打断
# 重拉均自愈（≈50s 重建到 ready）。
# 脚本纪律（2026-10-11 首两跑踩坑）：① `hdc shell` 不传递远端 rc（false 也返 0）
# —— 所有判据必须取**输出**本地判空, 禁用 if hdc-shell-grep; then; ② hilog 起读
# 先回放环形缓冲（可含一小时前旧 boot）—— 先 `hilog -r` 清环 + 按新 app pid 锚定;
# ③ 设备无 awk/tr —— pid 提取用 sed; ④ 停采集 kill -9 按 PID（pkill/SIGTERM
# 不可靠）, pid 列表必须空格连接一次性下发（换行会被 sh 当新命令执行）;
# ⑤ 别用 BRE `\|` 交替（toybox 不可靠）, 用 -E 或分开 grep。
#
# 用法 (仓库根目录):
#   bash scripts/package.sh deploy <IP>            # 前置: 全新安装（首启窗口最宽）
#   bash scripts/d47-fault-inject.sh <IP> [输出目录, 默认 .temp/d47-inject]
# 产物: $OUT/device-capture-full.log (全量 hilog) + device-evidence/auto-retry.log
#   (提取的 [AUTO-RETRY] 行, auto-retry 判定器直接可判) + state-seq.log。
# 断言 (任一不满足 = 非零退出): ① state:failed 出现; ② [AUTO-RETRY] cold-start
#   engine fail 行出现; ③ 重试后新 boot 达 state:ready; ④ auto-retry 判定器
#   对提取日志判 PASS。
set -eo pipefail

IP="$1"
[ -n "$IP" ] || { echo "用法: $0 <设备IP:端口> [输出目录]"; exit 2; }
OUT="${2:-.temp/d47-inject}"
CAP=/data/local/tmp/cap-d47-retry.log
BUNDLE=app.hackeris.winehua

mkdir -p "$OUT/device-evidence"

# 取远端命令输出（rc 不可信, 一律取 stdout 本地判空）
rsh() { hdc -t "$IP" shell "$1" 2>/dev/null | tr -d '\r'; }
# 停采集: 括号技巧防自匹配; [h]ilog$ 只中裸读进程不碰 hilogd; 空格连接 pid
kill_captures() {
  local pids
  pids="$(rsh "ps -ef | grep -E 'cap-d47-retr[y]|[h]ilog\$' | sed -n 's/^ *[^ ]*  *\([0-9]*\).*/\1/p'" | tr '\n' ' ')"
  [ -n "$(echo $pids)" ] && rsh "kill -9 $pids" >/dev/null || true
}
app_pid() { rsh "pidof $BUNDLE"; }
# 提前退出（FAIL 路径）也要拉停采集 —— 首两跑在这里漏过清理
trap kill_captures EXIT

echo "[1/7] preflight"
hdc list targets | grep -q "$IP" || { echo "FAIL: 设备不在线 $IP"; exit 1; }
kill_captures
rsh "aa force-stop $BUNDLE" >/dev/null || true

echo "[2/7] 清环形缓冲 + 启动全量采集 $CAP"
rsh "hilog -r" >/dev/null
rsh "rm -f $CAP"
hdc -t "$IP" shell "setsid sh -c 'hilog > $CAP 2>&1' < /dev/null > /dev/null 2>&1 &"
sleep 2

echo "[3/7] 冷启应用（新进程）并在 wineboot 中途打断"
rsh "aa start -a EntryAbility -b $BUNDLE" >/dev/null
P1=""
for i in $(seq 1 20); do P1="$(app_pid)"; [ -n "$P1" ] && break; sleep 1; done
[ -n "$P1" ] || { echo "FAIL: aa start 后 20s 无 app 进程"; exit 1; }
echo "    #1 app pid=$P1"
WB_SEEN=0
for i in $(seq 1 90); do
  if [ -n "$(rsh "grep -a 'state:starting:wineboot' $CAP | grep -a ' $P1 ' | head -1")" ]; then
    WB_SEEN=1; break
  fi
  sleep 2
done
[ "$WB_SEEN" = 1 ] || { echo "FAIL: 180s 内未见本进程的 starting:wineboot"; exit 1; }
echo "    wineboot 阶段已进入 (iter=$i)，等 42s 至 wineboot 尾段（mono/msiexec 安装相）"
sleep 42
rsh "aa force-stop $BUNDLE" >/dev/null
for i in $(seq 1 10); do [ -z "$(app_pid)" ] && break; sleep 1; done
[ -z "$(app_pid)" ] || { echo "FAIL: force-stop 后 #1 进程仍在"; exit 1; }
echo "    #1 已打断（mid-wineboot）"

echo "[4/7] 重拉新进程，等重拉 boot 的 state:failed（≤360s）"
# 本节起的成败判据一律锚定重拉进程 $P2（脚本纪律 ②/③）: 清环后缓冲仍可能残留
# P1 打断前的中途行（如 P1 自身 failed/AUTO-RETRY），不锚定会张冠李戴。
sleep 2
rsh "aa start -a EntryAbility -b $BUNDLE" >/dev/null
P2=""
for i in $(seq 1 20); do P2="$(app_pid)"; [ -n "$P2" ] && break; sleep 1; done
[ -n "$P2" ] || { echo "FAIL: 重拉后 20s 无 app 进程"; exit 1; }
echo "    #2 app pid=$P2"
FAILED_SEEN=0
for i in $(seq 1 180); do
  if [ -n "$(rsh "grep -a 'state:failed' $CAP | grep -a ' $P2 ' | head -1")" ]; then
    FAILED_SEEN=1; break
  fi
  sleep 2
done
if [ "$FAILED_SEEN" = 1 ]; then
  echo "    state:failed 已出现: $(rsh "grep -a 'state:failed' $CAP | grep -a ' $P2 ' | head -1" | tail -c 120)"
else
  echo "FAIL: 重拉 boot 360s 内未 failed（干净恢复 = 本轮未触发, 重跑或换中断点）"
  exit 1
fi

echo "[5/7] 等 [AUTO-RETRY] 行与重试后 ready"
RETRY_SEEN=0
for i in $(seq 1 30); do
  if [ -n "$(rsh "grep -a 'AUTO-RETRY' $CAP | grep -a ' $P2 ' | head -1")" ]; then RETRY_SEEN=1; break; fi
  sleep 1
done
[ "$RETRY_SEEN" = 1 ] || { echo "FAIL: state:failed 后 30s 内无 [AUTO-RETRY] 行（止血未触发）"; exit 1; }
echo "    [AUTO-RETRY]: $(rsh "grep -a 'AUTO-RETRY' $CAP | grep -a ' $P2 ' | head -1" | tail -c 140)"
echo "    等重试 boot 至 ready (≤300s)"
READY_SEEN=0
for i in $(seq 1 150); do
  if [ -n "$(rsh "grep -a 'state:ready' $CAP | grep -a ' $P2 ' | head -1")" ]; then READY_SEEN=1; break; fi
  sleep 2
done
[ "$READY_SEEN" = 1 ] || { echo "FAIL: 重试后 300s 内未达 ready"; exit 1; }
echo "    ready 达成 (iter=$i)"

echo "[6/7] 停采集 + 全量拉取"
kill_captures
sleep 1
hdc -t "$IP" file recv "$CAP" "$OUT/device-capture-full.log"
S1="$(rsh "ls -l $CAP" | tr -s ' ' | cut -d' ' -f5)"
sleep 3
S2="$(rsh "ls -l $CAP" | tr -s ' ' | cut -d' ' -f5)"
[ "$S1" = "$S2" ] || { echo "FAIL: 采集未停 (尺寸仍在涨 $S1 -> $S2)"; exit 1; }
rsh "rm -f $CAP" >/dev/null

echo "[7/7] 提取证据 + 判定器复判"
grep -a "AUTO-RETRY" "$OUT/device-capture-full.log" > "$OUT/device-evidence/auto-retry.log" || true
grep -a "state:starting\|state:ready\|state:failed\|Launch-Async\|AUTO-RETRY" \
  "$OUT/device-capture-full.log" | grep -a -v "GUEST-FRAMES" > "$OUT/state-seq.log"
python3 - "$OUT" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "automation")
from checks.auto_retry import auto_retry
r = auto_retry({"run_dir": Path(sys.argv[1])})
print("    judge:", r["status"], "-", r["message"][:100])
sys.exit(0 if r["status"] == "PASS" else 1)
PY
echo "D47 [AUTO-RETRY] 故障注入验证 PASS（证据: $OUT/）"
