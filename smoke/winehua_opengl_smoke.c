/*
 * winehua_opengl_smoke.c — P0-GL-5: Windows(WGL) OpenGL 能力探针
 *
 * 为什么需要它: Steam CEF 报 "Requested GLES version (3.0) is greater than max
 * supported (2, 0)", 而 Host App EGL 与 VirGL Host EGL 实测都是 GLES 3.2。
 * 要判断天花板来自哪一层, 必须有一个**真正走 Windows GL 路径**的客户端:
 *   Windows OpenGL -> wine opengl32 -> WGL -> guest Mesa(virpipe) -> vtest
 *     -> virglrenderer(capset) -> Host EGL/GLES
 * 这个探针就是这条链的入口, 同时会触发 virglrenderer 的 capset 生成
 * (vrend_renderer_fill_caps), 于是三层能力可以对着同一份日志读。
 *
 * 输出: stdout + 与 exe 同目录的 gl-probe-<arch>.log (Wine stderr 可能被重定向,
 * 文件才是权威)。逐层给 PASS/FAIL, 便于当 gate 用。
 *
 * 阶段:
 *   GL0 CreateWindow + ChoosePixelFormat/SetPixelFormat + DescribePixelFormat
 *   GL1 wglCreateContext + MakeCurrent + GL_VERSION/RENDERER/VENDOR/GLSL
 *   GL2 wglCreateContextAttribsARB 逐档要 core context (4.6/4.3/3.3/3.2/3.0)
 *   GL3 glClear + glReadPixels 回读 (默认 framebuffer 真的能出像素)
 *   GL4 FBO 离屏渲染 + 纹理采样 (需要 >=3.0 的 FBO 入口)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001
#endif

/* mingw 的 GL/gl.h 只到 GL 1.1, 现代常量自己补 */
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER              0x8D40
#define GL_RENDERBUFFER             0x8D41
#define GL_RGBA8                    0x8058
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#endif

#ifndef ARCH_LABEL
#define ARCH_LABEL "unknown"
#endif

typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef void (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

static FILE *g_log;

static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 2] = 0;
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (g_log) {
        fputs(buf, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

// 与 exe 同目录的日志文件: 不依赖 CWD, 也不依赖 Wine 的 stderr 重定向。
static void OpenLog(void)
{
    char path[MAX_PATH];
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    strncat(path, "gl-probe-" ARCH_LABEL ".log", sizeof(path) - strlen(path) - 1);
    g_log = fopen(path, "a");
    if (g_log) Log("=== winehua_opengl_smoke arch=" ARCH_LABEL " log=%s ===", path);
}

static void LogGLString(const char *name, GLenum which)
{
    const GLubyte *s = glGetString(which);
    Log("GL-STR %s=%s", name, s ? (const char *)s : "(null)");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    OpenLog();
    Log("GL-CAP guest layer=wgl arch=%s begin", ARCH_LABEL);

    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = "winehua-opengl-smoke";
    ATOM cls = RegisterClassA(&wc);
    if (!cls) Log("GL0 register_class FAIL err=%lu (ok if already registered)", GetLastError());

    HWND hwnd = CreateWindowExA(0, "winehua-opengl-smoke", "winehua-opengl-smoke",
                                WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                                NULL, NULL, inst, NULL);
    if (!hwnd) {
        Log("GL0 create_window FAIL err=%lu", GetLastError());
        return 2;
    }
    HDC dc = GetDC(hwnd);
    Log("GL0 create_window PASS hwnd=%p dc=%p", (void *)hwnd, (void *)dc);

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int fmt = ChoosePixelFormat(dc, &pfd);
    Log("GL0 choose_pixel_format fmt=%d err=%lu", fmt, GetLastError());
    if (!fmt || !SetPixelFormat(dc, fmt, &pfd)) {
        Log("GL0 set_pixel_format FAIL err=%lu", GetLastError());
        return 3;
    }
    {
        PIXELFORMATDESCRIPTOR got;
        memset(&got, 0, sizeof(got));
        if (DescribePixelFormat(dc, fmt, sizeof(got), &got)) {
            Log("GL0 pixel_format color=%u alpha=%u depth=%u stencil=%u double=%d",
                got.cColorBits, got.cAlphaBits, got.cDepthBits, got.cStencilBits,
                (got.dwFlags & PFD_DOUBLEBUFFER) ? 1 : 0);
        }
    }
    Log("GL0 PASS");

    HGLRC ctx = wglCreateContext(dc);
    if (!ctx) {
        Log("GL1 wglCreateContext FAIL err=%lu", GetLastError());
        return 4;
    }
    if (!wglMakeCurrent(dc, ctx)) {
        Log("GL1 wglMakeCurrent FAIL err=%lu", GetLastError());
        return 5;
    }
    LogGLString("gl_version", GL_VERSION);
    LogGLString("gl_vendor", GL_VENDOR);
    LogGLString("gl_renderer", GL_RENDERER);
    LogGLString("glsl", GL_SHADING_LANGUAGE_VERSION);
    {
        const GLubyte *ext = glGetString(GL_EXTENSIONS);
        Log("GL1 extensions=%.500s", ext ? (const char *)ext : "(null)");
    }
    Log("GL1 PASS");

    PFN_wglCreateContextAttribsARB createAttribs =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    Log("GL2 wglCreateContextAttribsARB=%s", createAttribs ? "present" : "missing");
    if (createAttribs) {
        static const int wanted[][2] = {{4,6},{4,3},{3,3},{3,2},{3,0}};
        for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
            int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, wanted[i][0],
                WGL_CONTEXT_MINOR_VERSION_ARB, wanted[i][1],
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0, 0
            };
            HGLRC c = createAttribs(dc, NULL, attribs);
            Log("GL2 core_ctx %d.%d result=%s err=%lu", wanted[i][0], wanted[i][1],
                c ? "OK" : "FAIL", c ? 0UL : GetLastError());
            if (c) {
                if (wglMakeCurrent(dc, c)) {
                    LogGLString("core_gl_version", GL_VERSION);
                    LogGLString("core_gl_renderer", GL_RENDERER);
                }
                wglMakeCurrent(NULL, NULL);
                wglDeleteContext(c);
            }
        }
    }

    if (!wglMakeCurrent(dc, ctx)) {
        Log("GL3 make_current(legacy) FAIL err=%lu", GetLastError());
    } else {
        glViewport(0, 0, 320, 240);
        glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        Log("GL3 clear_readback rgba=%u,%u,%u,%u gl_error=0x%x",
            px[0], px[1], px[2], px[3], glGetError());
        Log("GL3 PASS");

        PFN_glGenFramebuffers genFbo = (PFN_glGenFramebuffers)wglGetProcAddress("glGenFramebuffers");
        PFN_glGenRenderbuffers genRbo = (PFN_glGenRenderbuffers)wglGetProcAddress("glGenRenderbuffers");
        PFN_glBindFramebuffer bindFbo = (PFN_glBindFramebuffer)wglGetProcAddress("glBindFramebuffer");
        PFN_glBindRenderbuffer bindRbo = (PFN_glBindRenderbuffer)wglGetProcAddress("glBindRenderbuffer");
        PFN_glRenderbufferStorage rboStorage = (PFN_glRenderbufferStorage)wglGetProcAddress("glRenderbufferStorage");
        PFN_glFramebufferRenderbuffer fboRbo = (PFN_glFramebufferRenderbuffer)wglGetProcAddress("glFramebufferRenderbuffer");
        PFN_glCheckFramebufferStatus checkFbo = (PFN_glCheckFramebufferStatus)wglGetProcAddress("glCheckFramebufferStatus");
        if (genFbo && genRbo && bindFbo && bindRbo && rboStorage && fboRbo && checkFbo) {
            GLuint fbo = 0, rbo = 0;
            genFbo(1, &fbo);
            bindFbo(GL_FRAMEBUFFER, fbo);
            genRbo(1, &rbo);
            bindRbo(GL_RENDERBUFFER, rbo);
            rboStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
            fboRbo(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
            GLenum status = checkFbo(GL_FRAMEBUFFER);
            Log("GL4 fbo status=0x%x (0x%x=complete)", status, (unsigned)GL_FRAMEBUFFER_COMPLETE);
            bindFbo(GL_FRAMEBUFFER, 0);
            Log("GL4 PASS");
        } else {
            Log("GL4 SKIP (FBO entry points missing)");
        }
    }

    SwapBuffers(dc);
    Log("GL-CAP guest layer=wgl arch=%s end", ARCH_LABEL);
    if (ctx) { wglMakeCurrent(NULL, NULL); wglDeleteContext(ctx); }
    if (dc) ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}
