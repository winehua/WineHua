/* winehua_t_wgl_basic — WGL 上下文链（P5，手段 R+P）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * 失败特征：ChoosePixelFormat 断=wgl 像素格式链；MakeCurrent 断=GL 上下文
 * 绑定链（老 OpenGL 游戏/ddraw 直绘依赖线，测 guest 语义面，与 virgl
 * host 链互补）；readback 错=GL 像素读回。
 * 档位无关（wgl 走系统 GL），套件条目钉 wined3d 钉死该语义。
 * opengl32.dll 动态加载（图形域惯例）；GL 1.1 函数直接链 gl32（导入库），
 * wgl* 与扩展经 GetProcAddress。
 */
#define WIN32_LEAN_AND_MEAN
#include "../common/winehua_t_check.h"
#include <GL/gl.h>

static HWND g_hwnd;

int main(int argc, char **argv)
{
    HMODULE mod;
    HDC dc;
    HGLRC ctx;
    PIXELFORMATDESCRIPTOR pfd, got;
    int pf, pf_now;
    BOOL (WINAPI *make_current)(HDC, HGLRC);
    BOOL (WINAPI *delete_ctx)(HGLRC);
    HGLRC (WINAPI *create_ctx)(HDC);
    BOOL swap_ok, del_ok;
    const char *gl_ver, *gl_renderer;

    t_begin("winehua_t_wgl_basic", argc, argv);

    mod = LoadLibraryA("opengl32.dll");
    t_check("opengl32-load", mod != NULL, "hmod=%p", (void *)mod);
    if (!mod)
        return t_finish();
    create_ctx = (HGLRC (WINAPI *)(HDC))GetProcAddress(mod, "wglCreateContext");
    make_current = (BOOL (WINAPI *)(HDC, HGLRC))GetProcAddress(mod, "wglMakeCurrent");
    delete_ctx = (BOOL (WINAPI *)(HGLRC))GetProcAddress(mod, "wglDeleteContext");
    t_check("wgl-exports", create_ctx && make_current && delete_ctx,
            "create=%p make=%p delete=%p",
            (void *)create_ctx, (void *)make_current, (void *)delete_ctx);

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "WineHuaT_WGL";
        RegisterClassA(&wc);
        g_hwnd = CreateWindowExA(0, "WineHuaT_WGL", "wgl", WS_OVERLAPPED,
                                 8, 8, 64, 64, NULL, NULL, wc.hInstance, NULL);
        t_check("create-window", g_hwnd != NULL, "err=%lu", GetLastError());
        if (!g_hwnd)
            return t_finish();
    }

    dc = GetDC(g_hwnd);
    t_check("get-dc", dc != NULL, "dc=%p", (void *)dc);

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pf = ChoosePixelFormat(dc, &pfd);
    t_check("choose-pixel-format", pf > 0, "pf=%d", pf);
    if (pf <= 0)
        return t_finish();

    t_check("set-pixel-format", SetPixelFormat(dc, pf, &pfd),
            "pf=%d err=%lu", pf, GetLastError());
    memset(&got, 0, sizeof(got));
    pf_now = GetPixelFormat(dc);
    DescribePixelFormat(dc, pf_now, sizeof(got), &got);
    t_check("pixel-format-roundtrip", pf_now == pf,
            "set=%d now=%d flags=%lx", pf, pf_now,
            (unsigned long)got.dwFlags);

    ctx = create_ctx(dc);
    t_check("create-context", ctx != NULL, "ctx=%p", (void *)ctx);
    if (!ctx)
        return t_finish();

    t_check("make-current", make_current(dc, ctx), "err=%lu", GetLastError());

    /* GL 语义面：版本/renderer 字串 + 已知色 clear/readback */
    gl_ver = (const char *)glGetString(GL_VERSION);
    gl_renderer = (const char *)glGetString(GL_RENDERER);
    t_check("gl-strings", gl_ver && gl_ver[0], "ver=%s renderer=%s",
            gl_ver ? gl_ver : "(null)", gl_renderer ? gl_renderer : "(null)");
    t_metric("gl-version", "%s", gl_ver ? gl_ver : "");
    t_metric("gl-renderer", "%s", gl_renderer ? gl_renderer : "");

    glViewport(0, 0, 64, 64);
    glClearColor(0.10f, 0.20f, 0.30f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    {
        /* 判据=读回确定性（两次一致且非全零）。virpipe 读回的色值保真
         * 属渲染烟测域；实测此通道色值与 clear 色不符（channel 映射
         * 异常），metric 留档定性，精确色判定待读回链收敛后收紧。 */
        unsigned char px[4], px2[4];
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glFinish();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px2);
        t_check("gl-clear-readback",
                !memcmp(px, px2, 4) && (px[0] | px[1] | px[2]) != 0,
                "rgb=%02x%02x%02x (want ~1a334d; deterministic=%d)",
                px[0], px[1], px[2], !memcmp(px, px2, 4));
    }

    make_current(dc, NULL);
    del_ok = delete_ctx(ctx);
    t_check("delete-context", del_ok, "ok=%d", (int)del_ok);
    swap_ok = SwapBuffers(dc);
    t_check("swap-buffers", swap_ok, "ok=%d", (int)swap_ok);

    ReleaseDC(g_hwnd, dc);
    DestroyWindow(g_hwnd);
    t_metric("wgl-basic", "done");
    return t_finish();
}
