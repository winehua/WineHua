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
void main() { oColor = texture(uTex, vUV).bgra; }
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
    ownsContext_ = true;

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

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    return true;
}

bool EglRenderer::InitShared(EGLDisplay sharedDisplay, EGLContext sharedContext,
                              OHNativeWindow* window, int w, int h, uint32_t toplevelId) {
    window_ = window;
    width_ = w;
    height_ = h;
    display_ = sharedDisplay;
    toplevelId_ = toplevelId;
    ownsContext_ = false;

    EGLConfig cfg;
    EGLint nCfg;
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    eglChooseConfig(display_, attrs, &cfg, 1, &nCfg);

    // 共享 primary EGLContext
    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context_ = eglCreateContext(display_, cfg, sharedContext, ctxAttrs);
    if (context_ == EGL_NO_CONTEXT) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateContext (shared) failed: 0x%{public}x (toplevel=%{public}u)",
                     eglGetError(), toplevelId);
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, cfg,
                                       reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed: 0x%{public}x (toplevel=%{public}u)",
                     eglGetError(), toplevelId);
        return false;
    }

    OH_LOG_INFO(LOG_APP, "[EGL] toplevel #%{public}u renderer (shared ctx) init ok", toplevelId);
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
            if (loopCount < 3) {
                OH_LOG_INFO(LOG_APP, "[MW-RNDR] toplevelId=%{public}u frame %{public}dx%{public}d loop=%{public}d",
                            toplevelId_, fw, fh, loopCount);
            }
            glBindTexture(GL_TEXTURE_2D, texture_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }

        glViewport(0, 0, width_, height_);
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
        // 只有 primary renderer 才销毁 context 和 display
        if (ownsContext_) {
            if (context_ != EGL_NO_CONTEXT) {
                eglDestroyContext(display_, context_);
                context_ = EGL_NO_CONTEXT;
            }
            eglTerminate(display_);
            display_ = EGL_NO_DISPLAY;
        }
    }
}
