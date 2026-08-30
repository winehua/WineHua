#!/bin/bash
# collect_proc_env_baseline.sh — 采集 wine 各进程启动参数/环境变量基线
#
# 两条采集通道:
#   1) app 侧快照 (wine_child.cpp dump_proc_snapshot): 每个经 NCP 发射点创建的
#      进程在 execve/box64_hmos_main 前, 把真实 argv+environ 追加到
#      沙箱 temp/proc_snapshot.log — 引擎链与直启程序的完整快照。
#   2) 设备端轮询 (proc_env_watch.sh): 每秒扫 /proc/<pid>/cmdline, 覆盖
#      wineserver fork 创建 (文件管理里双击启动) 的进程的 argv。
#      (其 environ 继承自 wineserver, 文档中以 wineserver 快照为基说明)
#
# 用法:
#   bash scripts/collect_proc_env_baseline.sh <device_ip:port> all
#   bash scripts/collect_proc_env_baseline.sh <device_ip:port> install
#   bash scripts/collect_proc_env_baseline.sh <device_ip:port> engine
#   bash scripts/collect_proc_env_baseline.sh <device_ip:port> game <windows.exe>
#   bash scripts/collect_proc_env_baseline.sh <device_ip:port> filemanager
#   bash scripts/collect_proc_env_baseline.sh <device_ip:port> summary
#
# 场景顺序建议: install → engine → game x2 → filemanager → summary
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEVICE="${1:?usage: $0 <device_ip:port> CMD [...]}"
CMD="${2:-summary}"
shift 2 || true

BUNDLE=app.hackeris.winehua
ABILITY=EntryAbility
# 设备端路径 (hdc 视角)
APP_TMP=/data/app/el2/100/base/app.hackeris.winehua/temp
SNAP_REMOTE=$APP_TMP/proc_snapshot.log
WATCH_REMOTE=/data/local/tmp/proc_env_watch.log
WATCH_PID_REMOTE=/data/local/tmp/proc_env_watch.pid
# 本地归档
ARCHIVE=$ROOT/.temp/proc_baseline
SNAP_LOCAL=$ARCHIVE/proc_snapshot.log
WATCH_LOCAL=$ARCHIVE/proc_watch.log

H="hdc -t $DEVICE"
h() { $H shell "$@"; }

check_device() {
  # 接受 IP 或 IP:PORT (hdc list targets 的 key 是 IP:PORT)
  if ! $H listtargets 2>/dev/null | grep -q "$DEVICE"; then :; fi
  if $H shell "echo ok" 2>/dev/null | grep -q ok; then
    echo "[device] $DEVICE 可达"
  else
    echo "[device] 不可达, 请在 hdc list targets 确认 key (含端口)" >&2
    exit 1
  fi
}

marker() { # marker <场景名> — 注意 | 需单引号保护 (设备 sh 会把裸 | 当管道)
  h "echo 'MARK|$1|$(date +%H%M%S)' >> $WATCH_REMOTE" || true
  echo "[marker] $1"
}

watch_start() {
  # hdc 不转发远端退出码, 判断一律看输出内容
  if h "cat $WATCH_PID_REMOTE >/dev/null 2>&1 && echo ALIVE" | grep -q ALIVE; then
    echo "[watch] 已在运行"
    return
  fi
  $H file send "$ROOT/scripts/proc_env_watch.sh" /data/local/tmp/proc_env_watch.sh >/dev/null 2>&1 || h "cat > /data/local/tmp/proc_env_watch.sh '$(cat "$ROOT/scripts/proc_env_watch.sh")'"
  ( $H shell "sh /data/local/tmp/proc_env_watch.sh" >/dev/null 2>&1 & )
  sleep 2
  echo "[watch] 启动完成"
  marker "@watch-start"
}

watch_stop() {
  h "kill \$(cat $WATCH_PID_REMOTE) 2>/dev/null" || true
  marker "@watch-stop"
  sleep 1
}

pull_all() {
  mkdir -p "$ARCHIVE"
  $H file recv "$SNAP_REMOTE" "$SNAP_LOCAL.new" >/dev/null 2>&1 || h "echo '(none)' > /dev/null"
  $H file recv "$WATCH_REMOTE" "$WATCH_LOCAL" >/dev/null 2>&1 || touch "$WATCH_LOCAL"
  # 快照文件是追加式, 拉回后全量保存
  if [ -s "$SNAP_LOCAL.new" ]; then mv "$SNAP_LOCAL.new" "$SNAP_LOCAL"; fi
  echo "[pull] snapshot=$(wc -l < "$SNAP_LOCAL" 2>/dev/null || echo 0) 行, watch=$(wc -l < "$WATCH_LOCAL") 行"
}

wait_engine_ready() { # 最大 360s, 判据: 沙箱 proc_snapshot.log 出现 tag=explorer
  local i=0
  while [ $i -lt 180 ]; do
    if h "grep -q 'tag=explorer' $SNAP_REMOTE 2>/dev/null && echo FOUND" | grep -q FOUND; then
      echo "[ready] 引擎就绪 (explorer 快照已落盘, ${i}x2s)"
      return 0
    fi
    i=$((i + 1)); sleep 2
  done
  echo "[ready] 超时: 未见 explorer 快照" >&2
  return 1
}

install_app() {
  local hap=${1:-$ROOT/entry/build/default/outputs/default/entry-default-signed.hap}
  echo "[install] 卸载旧包"
  h "bm uninstall -n $BUNDLE" || true
  echo "[install] 推送并安装 $hap"
  $H file send "$hap" /data/local/tmp/winehua-baseline.hap
  h "bm install -p /data/local/tmp/winehua-baseline.hap"
  echo "[install] 完成"
}

start_engine() { # 冷启 app (无参 = 正常模式, 引擎自动初始化)
  watch_start
  marker "engine"
  h "aa force-stop $BUNDLE" || true
  h "aa start -a $ABILITY -b $BUNDLE"
  wait_engine_ready
  sleep 5
  pull_all
}

start_game() { # 冷启 app 并直启指定游戏/烟测程序
  local exe="${1:?usage: game <windows.exe>}"
  watch_start
  marker "game:$exe"
  h "aa force-stop $BUNDLE" || true
  # --ps 一次只收一对键值, 重复给; 路径经设备 sh 单引号保护 \ 与空格
  h "aa start -a $ABILITY -b $BUNDLE --ps winehua.mode game --ps winehua.game_path '$exe'"
  local i=0
  while [ $i -lt 120 ]; do
    local base
    base=$(basename "$exe")
    if h "grep -q '$base' $WATCH_REMOTE 2>/dev/null && echo FOUND" | grep -q FOUND; then
      echo "[game] $base 已启动 (${i}x2s)"
      sleep 10
      break
    fi
    i=$((i + 1)); sleep 2
  done
  pull_all
}

filemanager_scene() { # 由用户操作: 在 app 内文件管理(explorer)里双击启动一个程序
  watch_start
  marker "filemanager"
  echo "[fm] 请在设备 app 的文件管理器中双击启动一个程序, 完成后按回车..."
  read -r _
  sleep 3
  pull_all
}

summary() {
  if [ ! -f "$SNAP_LOCAL" ] || [ ! -f "$WATCH_LOCAL" ]; then
    echo "[summary] 先跑过采集再执行" >&2
    exit 1
  fi
  python3 "$ROOT/scripts/proc_baseline_summarize.py" \
    --snapshot "$SNAP_LOCAL" --watch "$WATCH_LOCAL" \
    --device "$DEVICE" --out "$ROOT/docs/WINE_PROC_ENV_BASELINE.md"
  echo "[summary] 已生成 docs/WINE_PROC_ENV_BASELINE.md"
}

check_device
mkdir -p "$ARCHIVE"
case "$CMD" in
  install)    install_app ;;
  engine)     start_engine ;;
  game)       start_game "$1" ;;
  filemanager) filemanager_scene ;;
  summary)    summary ;;
  all)        install_app; start_engine; start_game "C:\\smoke\\x64\\winehua_graphics_smoke.exe"; start_game "C:\\smoke\\x64\\winehua_d3d_switch_cube.exe"; filemanager_scene; summary ;;
  *) echo "unknown cmd: $CMD" >&2; exit 1 ;;
esac
