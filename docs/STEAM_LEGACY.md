# Steam Legacy 内置说明

## 这是什么

应用库的 Steam 条目使用「Steam Legacy」预置客户端：`AppLibraryService.ensureSteamLegacy()`
在 wine prefix 首次就绪时，把 `rawfile/steam-legacy.zip` 流式解压到 prefix 的
`drive_c/Program Files (x86)/Steam`（marker = `Steam/steam.exe` 幂等；分块拷贝，
避免 rawfile 一次性加载进内存）。

## 版本与来源

- 来源：archive.org 的 **2024-09-18 BETA 完整包**（Win7 兼容版，旧版 CEF）
- 客户端 build：**1726604483**
- 防自更新：包内 `steam.cfg` 版本锁，阻止客户端自我升级到现代版本
- 规模：716MB，6243 文件

## 为什么是 Legacy 而不是现代客户端

现代 Steam 客户端的 steamwebhelper 在 box64/wine 下存在结构性问题：
跨进程窗口收养死循环（steamui 主窗口 set_parent → destroy 循环）、CEF 消息泵
在 box64 下无法驱动（"未响应"误判）。经 12 轮真机迭代未能绕过，与 Winlator
社区 "steamwebhelper is not responding" 同病；Winlator 11.x 的最终解法同样是
提供 Steam Legacy 一键安装。Legacy 客户端（VGUI 界面、无 webhelper 硬依赖）
在 WineHua 上登录、商店浏览、下载均正常。

## 分发方式

zip 本体**不随仓库分发**（GitHub 100MB 单文件限制 + Valve 版权），已加入
`.gitignore`。需要者自备 zip 放到
`entry/src/main/resources/rawfile/steam-legacy.zip` 后构建 HAP；
文件缺失时 `ensureSteamLegacy()` 打 warning 跳过，应用其余功能不受影响
（Steam 条目点击会因文件未落地而启动失败）。

## 推荐启动参数

`-cef-single-process -cef-disable-gpu`（应用库条目 launchArgs 已持久化，
真机验证可用）。使用中避免在客户端内触发自更新/文件校验类操作，以免破坏
`steam.cfg` 版本锁导致升级到现代版本。
