#!/usr/bin/env python3
"""proc_baseline_summarize.py — 把采集的进程快照汇总为基线文档 (markdown)

输入:
  --snapshot  沙箱 temp/proc_snapshot.log (app 侧 dump_proc_snapshot 的追加式输出)
  --watch     设备端 /proc 轮询日志 (可选, 纯快照采集时不传)
  --out       输出 markdown 路径
  --device    设备标识 (写入文档头部)

快照格式 (wine_child.cpp / virgl_child.cpp dump_proc_snapshot):
  === SNAPSHOT pid=123 tag=wineserver
  argv: a b c
  env K=V
  ...
  === END

去重规则: 同名进程 (tag) 在会话中可能启动多次, 文档以最后一次为准
(完整时序记录保留在 .temp/proc_baseline/proc_snapshot.log)。
"""
import argparse
import datetime
import os
import re

SNAPSHOT_RE = re.compile(r"^=== SNAPSHOT pid=(\d+) tag=(\S+)")


# ---------- 解析 ----------

def parse_snapshot(path):
    """返回 [ {pid, tag, argv: [..], env: {key: val}} ]"""
    recs = []
    if not os.path.exists(path):
        return recs
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8", "replace")
    cur = None
    for line in text.splitlines():
        m = SNAPSHOT_RE.match(line)
        if m:
            cur = {"pid": m.group(1), "tag": m.group(2), "argv": [], "env": {}}
            continue
        if line == "=== END":
            if cur:
                recs.append(cur)
                cur = None
            continue
        if cur is None:
            continue
        if line.startswith("argv:"):
            cur["argv"] = line[5:].split()
        elif line.startswith("env "):
            kv = line[4:]
            if "=" in kv:
                k, v = kv.split("=", 1)
                cur["env"][k] = v
    return recs


def dedup_last(recs):
    """按 tag 去重, 保留最后一次出现的记录 (dict 覆盖值但保首现顺序)."""
    out = {}
    for r in recs:
        out[r["tag"]] = r
    return list(out.values())


def parse_watch(path):
    """设备端轮询日志 [ (scene, pid, args_str) ]; 无文件返回 []."""
    items = []
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return items
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    scene = "pre"
    for line in text.splitlines():
        if line.startswith("MARK|"):
            scene = line.split("|", 2)[1]
            continue
        if "|" not in line:
            continue
        pid, argv = line.split("|", 1)
        if pid.isdigit():
            items.append((scene, pid, argv))
    return items


# ---------- 环境变量分类 ----------

GRAPHICS_MARKERS = [
    "WAYLAND", "DISPLAY", "EGL", "GL_", "MESA", "VK_", "VKR_", "VN_",
    "LIBGL", "DRM_", "VULKAN", "DXVK_", "VKD3D_", "WINEHUA_VKR_",
    "WINEHUA_DXVK_", "XR_", "X11", "GDK_", "VTEST_", "VIRGL",
]
AUDIO_MARKERS = ["PULSE", "PIPEWIRE", "ALSA", "SOUND", "AUDIO", "OHOS_AUDIO"]
WINE_MARKERS = ["WIN", "WINE", "WINESERVER"]
BOX64_MARKERS = ["BOX64", "USE_LIBBOX64"]


def classify(key):
    u = key.upper()
    if any(m in u for m in BOX64_MARKERS):
        return "Box64"
    if key in ("WINEPREFIX",) or any(m in u for m in WINE_MARKERS):
        return "Wine"
    if any(m in u for m in AUDIO_MARKERS):
        return "Audio"
    if any(m in u for m in GRAPHICS_MARKERS):
        return "Graphics"
    return "System"


def fmt_env(env):
    """按分类展开 env"""
    groups = {}
    for k, v in sorted(env.items(), key=lambda kv: kv[0]):
        groups.setdefault(classify(k), []).append((k, v))
    order = ["Wine", "Graphics", "Audio", "Box64", "System"]
    out = []
    for g in order:
        if g not in groups:
            continue
        lines = [f"**{g}** ({len(groups[g])} 项):"]
        for k, v in groups[g]:
            lines.append(f"- `{k}={v}`")
        out.append("\n".join(lines))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--watch", default="")  # 可选, 纯 app 侧快照不传
    ap.add_argument("--device", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    recs = parse_snapshot(args.snapshot)
    procs = dedup_last(recs)
    watch = parse_watch(args.watch)

    lines = []
    w = lines.append
    w("# Wine 进程启动参数与环境变量基线")
    w("")
    w("> 自动生成: `scripts/proc_baseline_summarize.py`; 采集侧见 `wine_child.cpp` / `virgl_child.cpp` 的 `dump_proc_snapshot`")
    w("")
    w(f"- **采集时间**: {datetime.date.today().isoformat()}")
    w(f"- **设备**: `{args.device}`")
    w(f"- **代码位点**: master `90edaae` + 进程快照采集补丁 (`wine_child.cpp` + `virgl_child.cpp` 的 `dump_proc_snapshot`)")
    w("- **采集原理**: wine 的全部 guest 进程创建 (UI 直启 / 应用库点击 / 文件管理 explorer 里双击 /")
    w("  ShellExecute) 统一经 Process Broker → 系统 `StartNativeChildProcess` → NCP 发射点")
    w("  (`wine_child.cpp` `Main`/`WineserverMain`, `virgl_child.cpp` `Main`/`NativeChildProcess_MainProc`),")
    w("  发射瞬间把真实 `argv` + `environ` 逐条追加到沙箱 `temp/proc_snapshot.log`。")
    w("- **去重规则**: 同名进程在会话中可能多次启动, 本文档**以最后一次启动为准**;")
    w("  完整时序记录在 `.temp/proc_baseline/proc_snapshot.log`。")
    w("")
    w(f"共采集 **{len(recs)}** 条发射记录, 去重后 **{len(procs)}** 个进程.  ")
    w("")

    w("## 进程清单 (按启动顺序)")
    w("")
    w("| 序号 | 进程 tag | pid | argv |")
    w("|------|----------|-----|------|")
    for i, r in enumerate(procs, 1):
        argv_s = " ".join(r["argv"])[:110]
        argv_s = argv_s.replace("|", "\\|")
        w(f"| {i} | `{r['tag']}` | `{r['pid']}` | `{argv_s}` |")
    w("")

    for r in procs:
        w(f"### `{r['tag']}` (pid `{r['pid']}`)")
        w("")
        w("**argv**:")
        w("")
        w("```")
        w(" ".join(r["argv"]))
        w("```")
        w("")
        w(f"**environ ({len(r['env'])} 项)**")
        w("")
        w(fmt_env(r["env"]))
        w("")

    # ---- 环境变量矩阵 ----
    matrix = {}
    for r in procs:
        for k, v in r["env"].items():
            matrix.setdefault(k, []).append({"tag": r["tag"], "pid": r["pid"], "val": v})

    w("## 附录 A — 环境变量矩阵 (去重后进程, 按 KEY 分组)")
    w("")
    for k in sorted(matrix.keys()):
        entries = matrix[k]
        w(f"### `{k}`")
        w("")
        for e in entries:
            w(f"- `{e['tag']}@pid{e['pid']}`: `{e['val'][:160]}`")
        w("")

    gkeys = sorted(k for k in matrix if classify(k) == "Graphics")
    w("## 附录 B — 图形相关环境变量 (Graphics 类)")
    w("")
    if gkeys:
        for k in gkeys:
            entries = matrix[k]
            unique = sorted({e["val"] for e in entries})
            w(f"- `{k}` = `{'` | `'.join(unique)}`  (出现 {len(entries)} 次)")
    else:
        w("(无)")
    w("")
    akeys = sorted(k for k in matrix if classify(k) == "Audio")
    w("## 附录 C — 音频相关环境变量 (Audio 类)")
    w("")
    if akeys:
        for k in akeys:
            entries = matrix[k]
            unique = sorted({e["val"] for e in entries})
            w(f"- `{k}` = `{'` | `'.join(unique)}`  (出现 {len(entries)} 次)")
    else:
        w("(无)")
    w("")

    if watch:
        w("## 附录 D — 设备端 /proc 轮询记录")
        w("")
        w("> 通道 B (可选): 每秒扫 `/proc/<pid>/cmdline` 得到宿主视角 argv;")
        w("> 该通道在确认 NCP 全量覆盖后仅作交叉验证。")
        w("")
        for (scene, pid, argv) in watch:
            w(f"- `{scene}` pid `{pid}`: `{argv[:160]}`")
        w("")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"wrote {args.out}: {len(recs)} recs → {len(procs)} procs, watch {len(watch)} lines")


if __name__ == "__main__":
    main()
