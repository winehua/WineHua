# 真机探针（GDI 文本度量 / COM-RPC）

两个 x86-64 PE，通过 WineHua 现成的 `winehua.mode=game` 通道运行，**不需要改 HAP**。
结果直接写文件（绕开日志通道）：`C:\windows\temp\<name>.txt`，并复制一份到 `Z:\`。

## fontprobe.c

用途：排除「GDI 文本度量本身坏掉」这条假设。
逐项检查 `EnumFontFamiliesExW` / `CreateFontIndirectW` / `GetTextFaceW` /
`GetTextMetricsW` / `GetTextExtentPoint32W` / `GetTextExtentExPointW` /
`GetGlyphIndicesW`，覆盖 ASCII / 中文 / 空串与多种 charset。

2026-09-12 结论：**这些调用全部成功**（310 行输出，无一处 `ok=0`）。
所以 Steam 的断言不在 GDI 这条路径上——实际在 DirectWrite，见
`../../ohos-rpc-stubless-arm64ec-crash.md`。

## comprobe.c

用途：COM / RPC / 服务控制共 16 步探针，是
**RPC stubless 代理在 ARM64EC 下崩溃的最小复现**。
每一步先写标记再调用，因此进程死亡时最后一个标记就是崩溃点。

2026-09-12 结论：第 9 步 `OpenSCManagerW` 必崩（`L""` 与 `NULL` 都一样），
前 8 步（COM 创建 / UUID / RPC 绑定 / 释放）全部正常。

## 构建

```sh
CC=<llvm-mingw>/bin/x86_64-w64-mingw32-gcc
$CC -O1 -municode -mwindows -o fontprobe.exe fontprobe.c -lgdi32 -luser32
$CC -O1 -municode -mwindows -o comprobe.exe comprobe.c \
    -lole32 -loleaut32 -lrpcrt4 -ladvapi32 -lshlwapi -luuid
```

（本目录下的 `*.exe` 是本地构建产物，未入 git。）

## 运行

```sh
# 1. 推送探针到共享目录（= guest 的 Z:）
hdc file send comprobe.exe \
  /storage/media/100/local/files/Docs/Download/app.hackeris.winehua/games/comprobe.exe

# 2. 冷启动后用 game 通道拉起
hdc shell "aa force-stop app.hackeris.winehua"
hdc shell "aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game --ps winehua.game_path 'Z:\\games\\comprobe.exe' \
  --ps winehua.run_id comprobe-1"

# 3. 读结果
hdc shell "cat '/data/app/el2/100/base/app.hackeris.winehua/files/.wine/drive_c/windows/temp/comprobe.txt'"
```

要同时抓 Wine 侧日志，在 `aa start` 上加
`--ps winehua.d3d_env_json '<URL 编码的 [{"key":"WINEDEBUG","value":"err+all,warn+all,+rpc"}]>'`，
然后读设备上的
`/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_YYYYMMDD.log`
（这是 `wine_child.cpp` 的 stderr 落盘通道，按 `=== PID=… entryParams=… ===` 分段）。
