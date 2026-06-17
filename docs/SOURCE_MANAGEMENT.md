# Wine 源码管理

> 更新: 2026-06-18

## 策略

Wine 和 Box64 有定制修改，通过 GitHub fork + git submodule 管理。

| 组件 | submodule 路径 | fork |
|------|---------------|------|
| Wine | `thirdparty/wine` | `honwine/wine` (fork from `wine-mirror/wine`) |
| Box64 | `thirdparty/box64` | `honwine/box64` (fork from `ptitSeb/box64`) |

其余 submodule (freetype, wayland, libxkbcommon, xkeyboard-config 等) 无定制修改，直接跟踪上游 release tag。

## Fetch 上游更新

```bash
cd thirdparty/wine

# Wine fork 保留 upstream remote
git fetch upstream
git merge upstream/master        # 合并上游
# 解决冲突...
git push origin ohos-port
```

## 新增 submodule 依赖

```bash
# 1. 添加 submodule (跟踪上游 release tag)
git submodule add <url> thirdparty/<name>
cd thirdparty/<name>
git checkout <tag>

# 2. 添加构建脚本 scripts/build_<name>.sh
# 3. 添加到 scripts/build_deps.sh
# 4. 添加到 scripts/assemble.sh (如果需要)
```
