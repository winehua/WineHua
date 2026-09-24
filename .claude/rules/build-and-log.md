# 构建与日志

操作要点在这里。详细的日志标签表、沙箱路径对照、崩溃定位步骤见
`docs/debugging/observability.md`，常用命令速查见 `docs/cheatsheet.md`。

## 构建

根目录 `Makefile` 是**唯一**构建入口。

```bash
make NATIVE_ARCH=arm64-v8a        # 完整构建（改了 wine 源码，或不确定改了什么）
make NATIVE_ARCH=arm64-v8a hap    # 只出 HAP（只改 ArkTS 或 entry/src/main/cpp/）
make NATIVE_ARCH=x86_64           # 模拟器 / x86_64 设备
```

- **改了 `thirdparty/wine/` 下的源码必须完整构建**：合成阶段要重新打包
  `wine-data.zip`，只跑 `hap` 装上去的还是旧引擎。
- Wine 构建用 stamp 文件 + `find -newer` 判断源码变更（stamp 在
  `build/.stamps/wine-arm64-v8a`）。
- 构建目录按架构隔离，不要混用。

## 部署

```bash
bash scripts/package.sh deploy <设备IP>      # 卸载 + 推送 + 安装
```

改过 Wine 之后还要清设备上的引擎数据，让它重新解压：

```bash
H="hdc -t <设备IP>"
$H shell "rm -rf /data/app/el2/100/base/app.hackeris.winehua/files/.wine \
                 /data/app/el2/100/base/app.hackeris.winehua/files/wine"
```

- **部署前先确认分支和版本**：设备上装的是哪个分支的构建，就在哪个分支构建。
  跨分支安装会被系统按「版本降级」拒绝（版本号在 `AppScope/app.json5`）。
- 改 Wine 后首次启动会弹「引擎有更新」，**必须点「立即应用」→「确定」**，
  否则引擎不启动。
- 设备清单、各设备的限制见 `docs/assets/devices.md`。

## 日志

| 看什么 | 在哪 |
|---|---|
| 应用日志（界面层 + 原生） | `hdc -t <IP> hilog` |
| Wine 内部输出、box64 崩溃现场 | 沙箱的 `temp/wine_stderr_YYYYMMDD.log` |
| 渲染器宿主日志 | 沙箱的 `cache/winehua_virgl_host.log` |

三个必须知道的坑：

1. **日志缓冲区只留两三分钟**，要留证据必须落盘：
   `hdc -t <IP> shell "setsid sh -c 'hilog -t app > /data/local/tmp/capture.log' &"`。
2. **浮点参数要加 `%{public}`**，否则打出来是 `<private>`，数字看不到。
3. **有些输出只在 Wine 的 stderr 文件里**（比如 box64 崩溃现场），系统日志里
   搜不到——搜不到不代表没发生。

日志标签表、按链路过滤的写法、崩溃定位四步：`docs/debugging/observability.md`。

## 输入事件调试流程

1. 构建 + 部署 + 启动：

   ```bash
   make NATIVE_ARCH=arm64-v8a hap && bash scripts/package.sh deploy <IP>
   hdc -t <IP> shell "aa start -a EntryAbility -b app.hackeris.winehua"
   ```

2. 后台持续采集，**每个问题场景单独一个文件**：

   ```bash
   : > .temp/live-monitor.log
   hdc -t <IP> hilog 2>/dev/null | grep --line-buffered -E \
     'CLICK-PIPE|KBD-PIPE|InjectEnter|InjectButton|InjectKey|InjectAxis|DROPPED|Seat\].*ERR' \
     >> .temp/live-monitor.log
   ```

3. 在设备上操作，复现问题。
4. 停止采集，按链路分段过滤分析（链路划分见 `docs/architecture/input.md`）。

**死锁类问题：卡死前的最后一条日志就是最后一跳，问题在它后面那一步。**

## 相关文档

- `docs/cheatsheet.md` — 常用命令速查
- `docs/debugging/observability.md` — 日志标签表、沙箱路径、崩溃定位
- `docs/debugging/troubleshooting.md` — 按现象查排查方向
- `docs/build/guide.md` — 构建流程与产物说明
