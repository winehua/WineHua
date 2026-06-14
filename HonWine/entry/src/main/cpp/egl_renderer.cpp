#include "egl_renderer.h"
#include "wayland_server.h"
#include "fps_counter.h"
#include <vector>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "WL_EGL"
#include <hilog/log.h>

// ── 全屏 quad 着色器 (Wayland ARGB = BGRA 内存序, 像素着色器中 swizzle) ──
static const char* kVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 0, 1); }
)";

static const char* kFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uTex;
void main() { oColor = vec4(texture(uTex, vUV).bgr, 1.0); }
)";

static GLuint CompileShader(GLenum type, const char* src) {
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

bool EglRenderer::Init(OHNativeWindow* window, int w, int h) {
    window_ = window;
    width_ = w;
    height_ = h;

    // 1. EGL display + context
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglGetDisplay failed");
        return false;
    }
    EGLint major, minor;
    if (!eglInitialize(display_, &major, &minor)) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglInitialize failed: 0x%{public}x", eglGetError());
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[EGL] %{public}d.%{public}d (primary)", major, minor);

    EGLConfig cfg;
    EGLint nCfg;
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    eglChooseConfig(display_, attrs, &cfg, 1, &nCfg);

    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctxAttrs);

    // OHOS: EGLNativeWindowType = OHNativeWindow* (cast to unsigned long)
    surface_ = eglCreateWindowSurface(display_, cfg,
                                       reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed: 0x%{public}x", eglGetError());
        return false;
    }
    {
        EGLint sw = 0, sh = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &sw);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &sh);
        OH_LOG_INFO(LOG_APP, "[EGL-Init] primary: req=%{public}dx%{public}d eglSurface=%{public}dx%{public}d", w, h, sw, sh);
    }

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    return true;
}

void EglRenderer::RenderLoop() {
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglMakeCurrent failed: 0x%{public}x", eglGetError());
        return;
    }

    // 2. 着色器
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFS);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    // 3. 全屏 quad VBO
    float quad[] = {
        -1,-1, 0,1,   1,-1, 1,1,   -1, 1, 0,0,
         1,-1, 1,1,   1, 1, 1,0,   -1, 1, 0,0,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    // 4. 纹理 (初始空)
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 5. 渲染循环: 60fps, 每帧按 toplevelId 取帧
    FpsCounter fps("render");
    std::vector<uint8_t> px;
    int fw = 0, fh = 0;
    int loopCount = 0;

    while (running_) {
        bool haveFrame = false;
        if (toplevelId_ != 0) {
            haveFrame = WaylandServer::GetInstance()->TakeToplevelFrame(toplevelId_, px, fw, fh);
        } else {
            haveFrame = WaylandServer::GetInstance()->TakeFrame(px, fw, fh);
        }

        if (haveFrame && fw > 0 && fh > 0) {
            glBindTexture(GL_TEXTURE_2D, texture_);
            int rowLen = (int)px.size() / fh / 4;
            if (rowLen != fw) {
                OH_LOG_WARN(LOG_APP, "[MW-RNDR] UNPACK_ROW_LENGTH rowLen=%{public}d fw=%{public}d px=%{public}zu fh=%{public}d",
                            rowLen, fw, px.size(), fh);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            if (rowLen != fw) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
        }

        // 计算 letterbox 视口: 保持内容宽高比, 居中于 EGL surface
        // 注意: 即使未取到新帧, 上一帧的 fw/fh 仍有效
        int vpX = 0, vpY = 0, vpW = width_, vpH = height_;
        if (fw > 0 && fh > 0) {
            float contentAspect = (float)fw / (float)fh;
            float surfaceAspect = (float)width_ / (float)height_;
            if (contentAspect > surfaceAspect) {
                // 内容更宽 → fit by width, 上下留黑边
                vpW = width_;
                vpH = (int)(width_ / contentAspect + 0.5f);
                vpY = (height_ - vpH) / 2;
            } else {
                // 内容更高 → fit by height, 左右留黑边
                vpH = height_;
                vpW = (int)(height_ * contentAspect + 0.5f);
                vpX = (width_ - vpW) / 2;
            }
        }
        glViewport(vpX, vpY, vpW, vpH);

        // 诊断: viewport vs frame vs surface
        if (loopCount < 10) {
            EGLint surfW = 0, surfH = 0;
            eglQuerySurface(display_, surface_, EGL_WIDTH, &surfW);
            eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfH);
            OH_LOG_INFO(LOG_APP, "[MW-RNDR] diag#%{public}d tl=%{public}u have=%{public}d vp=(%{public}d,%{public}d %{public}dx%{public}d) surf=%{public}dx%{public}d frame=%{public}dx%{public}d px=%{public}zu",
                        loopCount, toplevelId_, haveFrame ? 1 : 0,
                        vpX, vpY, vpW, vpH, surfW, surfH, fw, fh, px.size());
        }
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        eglSwapBuffers(display_, surface_);
        fps.Tick();
        loopCount++;
        usleep(16667); // ~60fps
    }
}

void EglRenderer::Shutdown() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        // 每个 renderer 独立 EGLContext, 各自销毁
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
}
