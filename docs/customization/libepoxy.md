# libepoxy 定制

> 适用场景：libepoxy 初始化失败（所有 GL 调用启动即挂）时；随 wine 顺带升级该模块时。
> 基线：d1f952c（2026-04-03，upstream master merge-base，winehua 独有 1 commit：2597254）
> 生成：2026-08-01，2026-09-25 核实（与 `git show 2597254` 逐行对照）
> 说明：libepoxy 侵入度极浅（贴上游头），合并时可随 wine 顺带更新

## 变更总览

- 修改文件：1（`src/dispatch_common.c`，+5 行）；新增文件：0

## 变更明细

### src/dispatch_common.c: 平台库名解析宏
- **解决的问题**：libepoxy 运行时通过 `dlopen` 按平台宏链选择 EGL/GLES 库名。鸿蒙没有 Android 式的 `libGLESv1_CM.so`/`libGLESv2.so` 分离，EGL 与 GLES 统一由系统提供 `libEGL.so` / `libGLESv3.so`（兼容 GLES 1/2/3 API）。不新增 `__OHOS__` 分支会落入默认（Linux）分支（`libGL.so.1`/`libEGL.so.1` 等带版本后缀名），鸿蒙上 `dlopen` 失败 → libepoxy 初始化失败 → 所有 OpenGL 调用启动即挂。
- **解决思路**：在 `#ifdef` 链的 APPLE 之后、ANDROID 之前插入 `__OHOS__` 分支，重定义 4 个库名宏：`GLX_LIB`、`EGL_LIB`、`GLES1_LIB`、`GLES2_LIB` 全部指向 `libEGL.so` 或 `libGLESv3.so`（GLX_LIB 也重定义为 `libGLESv3.so`——鸿蒙没有 GLX，但宏必须有值）。宏链完整顺序：APPLE → **OHOS（插入）** → ANDROID → WIN32 → 默认。
- **依赖的上游行为**：`epoxy_platform` 的库名宏链。本变更只插入分支，不改变其它平台行为。
- **不变式**：鸿蒙上 4 个宏的解析结果必须能被 dlopen 打开；任何一项指向不存在的库都会让对应 API 的符号解析在启动时失败。
- **验证方法**：`python3 automation/smoke.py run --suite core` 的 OpenGL smoke x86/x64 覆盖此路径（GL 程序正常初始化渲染即通过）。
