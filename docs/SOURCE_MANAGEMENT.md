# Wine 源码管理

> 更新: 2026-06-22

## 策略

Wine 和 Box64 有定制修改，通过 GitHub fork + git submodule 管理。

| 组件 | submodule 路径 | fork | 开发分支 | 稳定分支 |
|------|---------------|------|---------|---------|
| Wine | `thirdparty/wine` | `winehua/wine` | `arm64-pad` | `ohos-port` |
| Box64 | `thirdparty/box64` | `winehua/box64` | `arm64-pad` | `ohos-port` |

其余 submodule (freetype, wayland, libxkbcommon, xkeyboard-config 等) 无定制修改，直接跟踪上游 release tag。

## Box64 共享库编译

arm64 Pad 下 Box64 编译为 .so 而非可执行文件，通过 cmake flag 控制：

```bash
cmake -DLIBBOX64_SO=ON ...
ninja box64_hmos_core     # 输出 box64.so
```

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
