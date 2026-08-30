#!/bin/sh
# proc_env_watch.sh — 设备端进程轮询器 (由 collect_proc_env_baseline.sh 拉起)
#
# 每秒扫描 /proc/<pid>/cmdline, 记录所有 wine/box64/explorer/smoke/.exe 相关进程。
# 覆盖走 wineserver fork 创建、不经过 NCP 发射点 (wine_child.cpp) 的进程 —
# 例如"文件管理(explorer) 里双击启动"的程序, 补上 app 侧快照的盲区。
# 同 pid 只记首次命中 (pid 复用率低, 会话内够用)。
#
# 输出行格式: <pid>|<argv 原文(换行转空格)\n>
OUT=/data/local/tmp/proc_env_watch.log
SEEN=/data/local/tmp/proc_env_watch.seen

echo $$ > /data/local/tmp/proc_env_watch.pid
: > "$SEEN"
: > "$OUT"

while true; do
  for p in $(ls /proc | grep '^[0-9][0-9]*$'); do
    cl=$(cat /proc/$p/cmdline 2>/dev/null) || continue
    [ -n "$cl" ] || continue
    case "$cl" in
      *wine*|*box64*|*explorer*|*smoke*|*.exe*|*graphics*|*d3d*) ;;
      *) continue ;;
    esac
    if ! grep -q "^${p}|" "$SEEN"; then
      echo "$p|${cl}" >> "$OUT"
      echo "${p}|" >> "$SEEN"
    fi
  done
  sleep 1
done
