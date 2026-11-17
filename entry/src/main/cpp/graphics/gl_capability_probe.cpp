/**
 * gl_capability_probe.cpp — P0-GL-1 Host EGL/GLES 能力探测 (只读, 详见头文件)
 *
 * 输出格式刻意做成 "GL-CAP <层> <项>=<值>" 的单行记录, 直接 grep 就能拼出
 * Host App / VirGL Host / capset 三层的对照表:
 *
 *   GL-CAP host  layer=app egl_vendor=... egl_version=... client_apis=...
 *   GL-CAP host  layer=app try=es2 result=PASS gl_version=... gl_renderer=...
 *   GL-CAP host  layer=app try=es3.0 result=PASS ...
 */
#include "gl_capability_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_GLCAP"
#include <hilog/log.h>

#ifndef EGL_CONTEXT_MINOR_VERSION_KHR
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#endif

namespace {

constexpr const char* kGlCapLogPath = "/data/storage/el2/base/temp/gl-capability.log";

// 单行落盘 + hilog。hilog 环形缓冲会滚掉早期记录, 文件才是权威。
void GlCapEmit(const char* line)
{
    int fd = open(kGlCapLogPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        size_t len = strlen(line);
        ssize_t n = write(fd, line, len);
        (void)n;
        close(fd);
    }
    OH_LOG_INFO(LOG_APP, "%{public}s", line);
}

const char* GlStr(GLenum name)
{
    const GLubyte* s = glGetString(name);
    return s ? reinterpret_cast<const char*>(s) : "(null)";
}

// 每个 client version 独立选 config (ES2 用 ES2 bit, ES3 用 ES3 bit),
// 并要求 PBUFFER, 这样能在不碰任何窗口 surface 的前提下 make current。
void TryClientVersion(EGLDisplay dpy, const char* label, EGLint clientVer, EGLint minorVer,
                      bool withMinor)
{
    EGLint cfgAttrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, (clientVer >= 3) ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint nCfg = 0;

    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &nCfg) || nCfg < 1) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GL-CAP host layer=app try=%s result=NO_CONFIG egl_error=0x%x\n",
                 label, eglGetError());
        GlCapEmit(buf);
        return;
    }

    const EGLint pbAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    if (surf == EGL_NO_SURFACE) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GL-CAP host layer=app try=%s result=NO_PBUFFER egl_error=0x%x\n",
                 label, eglGetError());
        GlCapEmit(buf);
        return;
    }

    EGLint ctxAttrs[5];
    int k = 0;
    ctxAttrs[k++] = EGL_CONTEXT_CLIENT_VERSION;
    ctxAttrs[k++] = clientVer;
    if (withMinor) {
        ctxAttrs[k++] = EGL_CONTEXT_MINOR_VERSION_KHR;
        ctxAttrs[k++] = minorVer;
    }
    ctxAttrs[k] = EGL_NONE;

    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GL-CAP host layer=app try=%s result=FAIL egl_error=0x%x\n",
                 label, eglGetError());
        GlCapEmit(buf);
        eglDestroySurface(dpy, surf);
        return;
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GL-CAP host layer=app try=%s result=MAKE_CURRENT_FAIL egl_error=0x%x\n",
                 label, eglGetError());
        GlCapEmit(buf);
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        return;
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "GL-CAP host layer=app try=%s result=PASS gl_version=%s gl_vendor=%s "
             "gl_renderer=%s glsl=%s\n",
             label, GlStr(GL_VERSION), GlStr(GL_VENDOR), GlStr(GL_RENDERER),
             GlStr(GL_SHADING_LANGUAGE_VERSION));
    GlCapEmit(buf);

    // 扩展串很长, 只留前面一段: 判断 GLES3 相关扩展 (framebuffer_object /
    // texture_storage / uniform_buffer_object / instanced_arrays) 在不在。
    if (clientVer >= 3) {
        const char* ext = GlStr(GL_EXTENSIONS);
        char buf2[600];
        snprintf(buf2, sizeof(buf2), "GL-CAP host layer=app try=%s extensions=%.480s\n", label, ext);
        GlCapEmit(buf2);
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
}

void ProbeOnBackgroundThread()
{
    GlCapEmit("GL-CAP host layer=app begin\n");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        GlCapEmit("GL-CAP host layer=app result=NO_DISPLAY\n");
        return;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "GL-CAP host layer=app result=INIT_FAIL egl_error=0x%x\n",
                 eglGetError());
        GlCapEmit(buf);
        return;
    }

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "GL-CAP host layer=app egl_init=%d.%d egl_vendor=%s egl_version=%s client_apis=%s\n",
             major, minor, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION),
             eglQueryString(dpy, EGL_CLIENT_APIS));
    GlCapEmit(buf);
    {
        const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
        char buf2[900];
        snprintf(buf2, sizeof(buf2), "GL-CAP host layer=app egl_extensions=%.800s\n", ext ? ext : "(null)");
        GlCapEmit(buf2);
    }

    TryClientVersion(dpy, "es2", 2, 0, false);
    TryClientVersion(dpy, "es3.0", 3, 0, false);
    TryClientVersion(dpy, "es3.1", 3, 1, true);
    TryClientVersion(dpy, "es3.2", 3, 2, true);

    // ES3 是否支持 surfaceless (CEF/ANGLE 走的就是无 surface 的 context)
    {
        const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
        bool surfaceless = ext && strstr(ext, "EGL_KHR_surfaceless_context") != nullptr;
        char buf2[200];
        snprintf(buf2, sizeof(buf2),
                 "GL-CAP host layer=app surfaceless_context=%d\n", surfaceless ? 1 : 0);
        GlCapEmit(buf2);
    }

    GlCapEmit("GL-CAP host layer=app end\n");
}

} // namespace

void WineHuaProbeHostGlCapability()
{
    const char* opt = getenv("WINEHUA_GL_PROBE");
    if (opt && opt[0] == '0') {
        OH_LOG_INFO(LOG_APP, "[GL-CAP] probe disabled (WINEHUA_GL_PROBE=0)");
        return;
    }

    static std::once_flag once;
    std::call_once(once, []() {
        std::thread(ProbeOnBackgroundThread).detach();
    });
}
