#include "egl_renderer.h"
#include "wayland_server.h"
#include "fps_counter.h"
#include <vector>
#include <mutex>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "WL_EGL"
#include <hilog/log.h>

// -- 共享 EGLDisplay: 整个进程只初始化一次, 避免反复 init/terminate 导致 GPU 驱动竞争 --
static EGLDisplay gSharedDisplay = EGL_NO_DISPLAY;
static std::once_flag gDisplayOnce;

float EglRenderer::globalDisplayScale_ = 1.0f;

EGLDisplay EglRenderer::GetSharedDisplay() {
    std::call_once(gDisplayOnce, []() {
        gSharedDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (gSharedDisplay == EGL_NO_DISPLAY) {
            OH_LOG_ERROR(LOG_APP, "[EGL] eglGetDisplay FAILED");
            return;
        }
        EGLint major, minor;
        if (!eglInitialize(gSharedDisplay, &major, &minor)) {
            OH_LOG_ERROR(LOG_APP, "[EGL] eglInitialize FAILED: 0x%{public}x", eglGetError());
            gSharedDisplay = EGL_NO_DISPLAY;
            return;
        }
        OH_LOG_INFO(LOG_APP, "[EGL] shared display init OK EGL %{public}d.%{public}d", major, minor);
    });
    return gSharedDisplay;
}

// -- 全屏 quad 着色器 (Wayland ARGB = BGRA 内存序, 像素着色器中 swizzle) --
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

    OH_LOG_INFO(LOG_APP, "[EGL] Init tl=%{public}u req=%{public}dx%{public}d", toplevelId_, w, h);

    // 1. 使用共享 EGLDisplay (全进程只 init 一次)
    display_ = GetSharedDisplay();
    if (display_ == EGL_NO_DISPLAY) {
        OH_LOG_ERROR(LOG_APP, "[EGL] shared display unavailable tl=%{public}u", toplevelId_);
        return false;
    }

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
        OH_LOG_ERROR(LOG_APP, "[EGL] eglCreateWindowSurface failed tl=%{public}u: 0x%{public}x", toplevelId_, eglGetError());
        return false;
    }
    {
        EGLint sw = 0, sh = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &sw);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &sh);
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u eglSurface %{public}dx%{public}d", toplevelId_, sw, sh);
    }

    running_ = true;
    thread_ = std::thread(&EglRenderer::RenderLoop, this);
    OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Init done, render thread started OK", toplevelId_);
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
    bool firstFrameLogged = false;

    OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u render loop started", toplevelId_);

    while (running_) {
        bool haveFrame = false;
        if (toplevelId_ != 0) {
            haveFrame = WaylandServer::GetInstance()->TakeToplevelFrame(toplevelId_, px, fw, fh);
        } else {
            haveFrame = WaylandServer::GetInstance()->TakeFrame(px, fw, fh);
        }

        if (haveFrame && fw > 0 && fh > 0) {
            // 存储帧尺寸供输入坐标转换
            frameW_ = fw;
            frameH_ = fh;
            if (!firstFrameLogged) {
                OH_LOG_INFO(LOG_APP, "[MW-RNDR] tl=%{public}u  FIRST FRAME %{public}dx%{public}d px=%{public}zu",
                            toplevelId_, fw, fh, px.size());
                firstFrameLogged = true;
            }
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

        // 获取 EGL surface 实际大小
        EGLint surfW = 0, surfH = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &surfW);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &surfH);
        if (surfW > 0 && surfH > 0) {
            width_ = surfW;
            height_ = surfH;
        }

        // Letterbox 视口: 保持 Wine 帧宽高比, 居中渲染, 左右或上下黑边
        if (fw > 0 && fh > 0 && width_ > 0 && height_ > 0) {
            float frameAspect = (float)fw / fh;
            float surfAspect = (float)width_ / height_;
            if (surfAspect > frameAspect) {
                // Surface 比帧更宽 -> 左右黑边
                vpH_ = height_;
                vpW_ = (int)(height_ * frameAspect);
                vpX_ = (width_ - vpW_) / 2;
                vpY_ = 0;
            } else {
                // Surface 比帧更高 -> 上下黑边 (常见: 手机竖屏)
                vpW_ = width_;
                vpH_ = (int)(width_ / frameAspect);
                vpX_ = 0;
                vpY_ = (height_ - vpH_) / 2;
            }
            glViewport(vpX_, vpY_, vpW_, vpH_);
        } else {
            glViewport(0, 0, width_, height_);
        }

        // 诊断: 前10帧详细打印 surface -> frame -> viewport 完整映射
        if (loopCount < 10) {
            int barTop = vpY_;
            int barBot = height_ - vpY_ - vpH_;
            int barLeft = vpX_;
            int barRight = width_ - vpX_ - vpW_;
            float sA = (float)width_ / height_;
            float fA = fw > 0 && fh > 0 ? (float)fw / fh : 0;
            OH_LOG_INFO(LOG_APP, "[MW-RNDR] diag#%{public}d tl=%{public}u surface=%{public}dx%{public}d(asp=%{public}.2f) frame=%{public}dx%{public}d(asp=%{public}.2f) vp=%{public}dx%{public}d+%{public}d,%{public}d bar=(L%{public}d R%{public}d T%{public}d B%{public}d)",
                        loopCount, toplevelId_,
                        width_, height_, sA, fw, fh, fA,
                        vpW_, vpH_, vpX_, vpY_,
                        barLeft, barRight, barTop, barBot);
        }

        // surface 变化时打印 XComponent → Wine 尺寸映射 (与 ArkTS MW-RESIZE 共用关键字)
        if ((width_ != lastLoggedW_ || height_ != lastLoggedH_) && loopCount >= 10) {
            lastLoggedW_ = width_;
            lastLoggedH_ = height_;
            OH_LOG_INFO(LOG_APP, "[MW-RESIZE] tl=%{public}u surface=%{public}dx%{public}d frame=%{public}dx%{public}d",
                        toplevelId_, width_, height_, fw, fh);
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
        // 不调 eglTerminate: 共享 display 由进程生命周期管理
        // 避免反复 init/terminate 导致 GPU 驱动竞争, 偶发性 SIGSEGV
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u Shutdown OK (display retained)", toplevelId_);
    }
    // surfaceId 创建的 native window 在这里销毁 (EglRenderer 持有 window_ 指针)
    if (window_) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
        OH_LOG_INFO(LOG_APP, "[EGL] tl=%{public}u native window destroyed", toplevelId_);
    }
}
