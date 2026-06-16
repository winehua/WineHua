#pragma once
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <atomic>
#include <cstdint>

// 最小 EGL 渲染器: 从 WaylandServer 取帧 -> GL 纹理 -> XComponent 上屏
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

    // 尺寸 getters (供输入坐标转换: 触控坐标 -> wine 内容坐标)
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    int GetFrameWidth() const { return frameW_; }
    int GetFrameHeight() const { return frameH_; }
    // Letterbox viewport (保持 Wine 帧宽高比居中渲染的视口)
    int GetVpX() const { return vpX_; }
    int GetVpY() const { return vpY_; }
    int GetVpW() const { return vpW_; }
    int GetVpH() const { return vpH_; }

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
    int frameW_ = 0, frameH_ = 0;  // Wine 帧内容尺寸 (坐标转换)
    int vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;  // Letterbox 视口 (保持宽高比)
    int bufW_ = 0, bufH_ = 0;  // 上次 SET_BUFFER_GEOMETRY 的值, 避免重复调用
    int lastLoggedW_ = 0, lastLoggedH_ = 0;  // 上次输出 resize 日志时的 surface 尺寸
    std::thread thread_;
    std::atomic<bool> running_{false};

    uint32_t toplevelId_ = 0;

    static float globalDisplayScale_;
};
