#include "shader_utils.h"

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_EGL"

namespace winehua {

const char* kFullscreenQuadVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0, 1); }
)";

const char* kFullscreenQuadFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uTex;
uniform float uForceOpaque;
void main() {
    vec4 t = texture(uTex, vUV);
    // uForceOpaque=1: XRGB 帧 (alpha 字节是垃圾, 强制不透明)
    // uForceOpaque=0: ARGB 帧 (layered/shaped 异型窗口, 透传预乘 alpha)
    oColor = vec4(t.bgr, uForceOpaque > 0.5 ? 1.0 : t.a);
}
)";

const char* kZeroCopyExternalFS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vUV;
out vec4 oColor;
uniform samplerExternalOES uTex;
uniform mat4 uTransform;
void main() {
    vec4 coord = uTransform * vec4(vUV, 0.0, 1.0);
    oColor = texture(uTex, coord.xy);
}
)";

// presenter 路径全屏 quad (gl_VertexID 大三角形): 覆盖整个视口的超采样
// 三角形, 无需 attribute/VBO; 片段直读 RGBA texture, 无 swizzle/forceOpaque
// (与 egl_renderer 的 kFullscreenQuadFS 语义不同 — 后者处理 Wayland ARGB=
// BGRA 内存序 + XRGB forceOpaque, 不可互换)。收编于此处统一存放, 行为平价。
const char* kPresentFullscreenQuadVS = R"(#version 300 es
out vec2 vTexCoord;
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 texcoords[3] = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    vTexCoord = texcoords[gl_VertexID];
})";

const char* kPresentFullscreenQuadFS = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec2 vTexCoord;
out vec4 outColor;
void main() { outColor = texture(uTexture, vTexCoord); }
)";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        OH_LOG_ERROR(LOG_APP, "[EGL] shader compile: %{public}s", log);
    }
    return s;
}

} // namespace winehua
