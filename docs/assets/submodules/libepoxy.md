# libepoxy 补丁清单

> 基线：d1f952c（2026-04-03，upstream/anholt mirror master merge-base，winehua 独有 1 commit）
> 生成：2026-08-01
> 说明：libepoxy 侵入度极浅（贴上游头），合并时可随 wine 顺带更新；本清单记录唯一独有 commit 的意图

## 变更总览

- 修改文件：1（src/dispatch_common.c，+5 行）
- 新增文件：0

## 变更明细

### src/dispatch_common.c: 平台库名解析宏
- **为什么存在**：libepoxy 在运行时通过 `dlopen` 按平台宏选择 EGL/GLES 库名（`#ifdef` 链：APPLE → ANDROID → 默认）。鸿蒙没有 Android 式的 `libGLESv1_CM.so`/`libGLESv2.so` 分离，EGL 与 GLES 统一由系统提供 `libEGL.so` / `libGLESv3.so`（兼容 GLES 1/2/3 API）。不新增 `__OHOS__` 分支会落入默认分支（Linux 名 `libGL.so.1`/`libEGL.so.1` 等），鸿蒙上 `dlopen` 失败导致 libepoxy 初始化失败。
- **依赖的上游行为**：`epoxy_platform` 的库名宏链（apple/win32/android/默认），本变更只是在该链上插入一个分支，不改变其它平台行为。
- **不变式**：鸿蒙上 `epoxy_egl_init` / GLES 库解析必须指向 `libEGL.so` 与 `libGLESv3.so`。丢失后果：libepoxy 加载不到 EGL/GLES 符号，所有 OpenGL 调用在启动时失败。
- **验证方法**：任何 OpenGL 程序能正常初始化并渲染（core 套件 `automation --suite core` 的 OpenGL smoke x86/x64 即回归此路径）。
