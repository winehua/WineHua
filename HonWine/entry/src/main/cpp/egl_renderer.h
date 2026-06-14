#pragma once
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <atomic>
#include <cstdint>

// 最小 EGL 渲染器: 从 WaylandServer 取帧 → GL 纹理 → XComponent 上屏
class EglRenderer {
public:
    // 主渲染器: 创建 EGL display + context (primary)
    bool Init(OHNativeWindow* window, int w, int h);
    // 共享渲染器: 共享 primary EGL context, 渲染指定 toplevel
    bool InitShared(EGLDisplay sharedDisplay, EGLContext sharedContext,
                    OHNativeWindow* window, int w, int h, uint32_t toplevelId);
    void Shutdown();

    EGLDisplay GetDisplay() const { return display_; }
    EGLContext GetContext() const { return context_; }
    uint32_t GetToplevelId() const { return toplevelId_; }
    void SetToplevelId(uint32_t id) { toplevelId_ = id; }
    bool IsValid() const { return running_; }

private:
    void RenderLoop();

    OHNativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    GLuint texture_ = 0;
    GLuint program_ = 0;
    GLuint vbo_ = 0;

    int width_ = 0, height_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};

    uint32_t toplevelId_ = 0;
    bool ownsContext_ = true;  // false if shared from primary
};
