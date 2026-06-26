# 源码管理

> 更新: 2026-06-26

## 目标

把所有带 OHOS / WineHua 定制改动的上游工程收敛到独立子工程里管理，主仓库只负责：

- 锁定子工程提交
- 保存集成脚本、打包逻辑、宿主侧代码
- 记录构建入口和运行时装配方式

这样后续迁移机器、重新 clone、回溯某次构建，都能直接定位到确切源码版本。

## 受控子工程

| 组件 | 本地路径 | GitHub 仓库 | 上游基线 | 管理方式 | 当前状态 |
| --- | --- | --- | --- | --- | --- |
| Wine | `thirdparty/wine` | `winehua/wine` | Wine upstream | fork + submodule | 已存在 |
| Box64 | `thirdparty/box64` | `winehua/box64` | Box64 upstream | fork + submodule | 已存在 |
| virglrenderer | `thirdparty/virglrenderer` | `winehua/virglrenderer` | freedesktop virglrenderer | fork + submodule | 需要创建远端 |
| Mesa (OHOS guest_gfx) | `thirdparty/mesa-ohos` | `winehua/mesa-ohos` | `openharmony/third_party_mesa3d` | fork + submodule | 需要创建远端 |
| libdrm (OHOS guest_gfx) | `thirdparty/libdrm-ohos` | `winehua/libdrm-ohos` | `openharmony/third_party_libdrm` | fork + submodule | 需要创建远端 |

其余 `wayland / wayland-protocols / libxkbcommon / freetype / libffi / libxml2 / xkeyboard-config` 暂时继续跟随上游，无项目定制补丁时不额外 fork。

## 图形链路的源码归属

### 应该放在子工程里的改动

- `virglrenderer` 上对 `external EGL without GBM`、`vtest`、`surfaceless EGL/GLES` 的适配
- OHOS guest Mesa / libdrm 上为 `virpipe`、Wayland EGL、打包布局所做的移植修改
- 未来如果有 `zink`、DX/WGL 兼容性补丁，优先也落在对应上游 fork 里

### 应该留在主仓库里的改动

- `entry/src/main/cpp/graphics_broker.*`
- `scripts/build_native.sh`
- `scripts/build_ohos_guest_gfx.sh`
- `scripts/build_guest_gfx.sh`
- `scripts/assemble.sh`
- HNP/HAP 组装、环境变量注入、运行时探测、诊断日志

### 不应作为源码真相的内容

- `prebuilt/guest_gfx/*`
- `entry/hnp/*`
- `out/*`

这些目录是构建/打包产物，可以提交用于联调，但不能代替源码仓库本身。

## 当前构建优先级

`guest_gfx` 源码解析顺序现在应当是：

1. `thirdparty/mesa-ohos`
2. `thirdparty/libdrm-ohos`
3. `tmp/` 下通过 `scripts/fetch_ohos_mesa.sh` 拉取的临时源码

也就是说，后续把 Mesa / libdrm 作为正式子工程接入后，现有构建脚本无需再次重写，只要主仓库 pin 到对应 gitlink 即可。

## 复现要求

### 推荐路径

- 主仓库提交精确 submodule 指针
- `thirdparty/virglrenderer` / `thirdparty/mesa-ohos` / `thirdparty/libdrm-ohos` 分别提交到自己的仓库
- 构建时优先使用 `thirdparty/` 下的受控源码

### fallback 路径

当子工程远端还没准备好时，允许使用官方仓库抓取，但必须显式 pin ref：

```bash
bash scripts/fetch_ohos_mesa.sh \
  --mesa-ref <mesa-commit-or-tag> \
  --libdrm-ref <libdrm-commit-or-tag>
```

`scripts/build_guest_gfx.sh` 会把 `mesa/libdrm` 的源码路径和 Git HEAD 写入 `prebuilt/guest_gfx/<arch>/BUILD_INFO.txt`，用于回溯产物来源。

## GitHub 组织侧建议

建议在 `https://github.com/winehua` 下补齐这三个仓库：

- `virglrenderer`
- `mesa-ohos`
- `libdrm-ohos`

建议分支策略：

- `main`: 对齐导入时的上游基线
- `ohos-port`: 长期集成分支
- `topic/*`: 临时功能分支，例如 `topic/virgl-egl-without-gbm`

## 提交流程

1. 先在子工程里提交真实源码改动。
2. 推送子工程分支。
3. 回到主仓库，只更新 submodule gitlink 和相关构建/装配脚本。
4. 若产物布局变化，再更新 `prebuilt/guest_gfx/README.md`、`docs/OPENGL_VIRGL_DESIGN.md`、`README.md`。

这样后续审查时可以明确区分：

- 哪些是上游 fork 的通用补丁
- 哪些是 WineHua 集成层改动
- 哪些只是某次构建产物
