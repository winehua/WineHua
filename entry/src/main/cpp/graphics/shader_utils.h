#pragma once

#include <GLES3/gl3.h>

namespace winehua {

// 全屏 quad 顶点着色器 (Wayland ARGB = BGRA 内存序, 像素着色器中 swizzle)
extern const char* kFullscreenQuadVS;

// 全屏 quad 片段着色器 (SHM 纹理路径)
extern const char* kFullscreenQuadFS;

// 全屏 quad 片段着色器 (zero-copy external OES 纹理路径)
extern const char* kZeroCopyExternalFS;

// 全屏 quad 顶点着色器 (presenter 路径, gl_VertexID 大三角形 — 无 attribute/VBO,
// 与 kFullscreenQuadVS 的 attribute 驱动方式不同, 用于 SurfaceQueueTarget 内嵌 blit)
extern const char* kPresentFullscreenQuadVS;
// 全屏 quad 片段着色器 (presenter 路径, 直读 RGBA texture — 无 BGR swizzle /
// uForceOpaque, 与 kFullscreenQuadFS 语义不同, 不可互换)
extern const char* kPresentFullscreenQuadFS;

GLuint CompileShader(GLenum type, const char* src);

} // namespace winehua
