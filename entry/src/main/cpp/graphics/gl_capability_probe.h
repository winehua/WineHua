#pragma once

/*
 * gl_capability_probe — P0-GL-1: Host EGL/GLES 真实能力探测 (2026-09-18)
 *
 * 背景: Steam win64 (native 模式) 的 CEF 明确报
 *     eglCreateContext: Requested GLES version (3.0) is greater than max supported (2, 0)
 * 而 WineHua app 自己建 EGL context 时用的是 EGL_CONTEXT_CLIENT_VERSION=3。
 * 所以要先把"Host 到底支持到哪一档"测出来, 再决定修 virglrenderer / capset / guest Mesa。
 *
 * 只读探测, 不改变任何运行路径:
 *   - 打印 EGL_VENDOR / EGL_VERSION / EGL_CLIENT_APIS / EGL_EXTENSIONS
 *   - 用同一套 config 依次尝试 ES2 / ES3.0 / ES3.1 / ES3.2 context
 *   - 每个成功的 context 打印 GL_VENDOR / GL_RENDERER / GL_VERSION / GL_SHADING_LANGUAGE_VERSION
 * 结果落盘 temp/gl-capability.log (与 virglrenderer 侧的 virgl 能力日志同一个文件,
 * 便于直接对照 "Host App EGL" vs "VirGL Host EGL"), 同时进 hilog。
 *
 * 开关: WINEHUA_GL_PROBE=0 关闭 (默认开启, 纯诊断, 单次执行)。
 */
void WineHuaProbeHostGlCapability();
