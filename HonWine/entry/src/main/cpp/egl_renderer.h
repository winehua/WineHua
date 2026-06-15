#pragma once
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <atomic>
#include <cstdint>

// 最小 EGL 渲染器: 从 WaylandServer 取帧 → GL 纹理 → XComponent 上屏
// 所有实例共享同一个 EGLDisplay (避免反复 init/terminate 导致 GPU 驱动竞争)
// 每个实例拥有独立的 EGLContext + EGLSurface
class EglRenderer {
public:
    // 获取/初始化共享的 EGLDisplay (首次调用时初始化, 线程安全)
    static EGLDisplay GetSharedDisplay();

    bool Init(OHNativeWindow* window, int w, int h);
    void Shutdown();

    uint32_t GetToplevelId() const { return toplevelId_; }
    void SetToplevelId(uint32_t id) { toplevelId_ = id; }
    void SetSize(int w, int h) { width_ = w; height_ = h; }
    bool IsValid() const { return running_; }

    static void SetGlobalDisplayScale(float s) { globalDisplayScale_ = s; }
    static float GetGlobalDisplayScale() { return globalDisplayScale_; }

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
    int bufW_ = 0, bufH_ = 0;  // 上次 SET_BUFFER_GEOMETRY 的值, 避免重复调用
    std::thread thread_;
    std::atomic<bool> running_{false};

    uint32_t toplevelId_ = 0;

    static float globalDisplayScale_;
};
