/*
 * ohos_egl_import_probe.c — host EGL OHOS 导入链探针 (M2-T2, spec §5 R-ZC ②)
 *
 * 裁决问题: wlroots gles2 渲染器加 OHOS 导入分支 (eglCreateImageKHR +
 * EGL_NATIVE_BUFFER_OHOS + glEGLImageTargetTexture2DOES) 在真机是否可用。
 * spec 口径: "API 存在已证、组合未验证" —— 本探针就是把组合真机验一遍。
 *
 * 链路: OH_NativeBuffer_Alloc (RGBA8888 64x64, CPU 读写 usage) → Map 写
 * 测试图案 → eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS) → 纹理绑定 →
 * FBO attach → glReadPixels 回读 → 与写入图案比对。
 *
 * 载体: 合成器 bring-up 内嵌 (display_compositor 调本文件唯一入口),
 * 标记文件 displayroute-egl-import-probe (files 根) 不存在时执行 (自缓存:
 * 一次性
 * 几 ms, 结论落盘后不再重跑; 重跑 = 删标记)。设备端只跑不判: 结论枚举
 * 写标记文件 + hilog, 判定在主机 (smoke check 读归档)。
 *
 * 假设声明 (本探针的裁决对象之一): EGL_NATIVE_BUFFER_OHOS 的
 * EGLClientBuffer 载荷 = OH_NativeBuffer*。现行代码零 OH_NativeBuffer
 * 导入命中 (spec §5 R-ZC: 旧合成器全程 OH_NativeImage 纹理模式), 此
 * 假设无在库先例 —— 探针失败时先核这条。
 */
#include "ohos_egl_import_probe.h"

#define LOG_TAG "egl-import-probe" /* hilog tag (OH_LOG_* 用编译期 LOG_TAG) */
#include <hilog/log.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <native_window/external_window.h>

/* 扩展函数不在链接桩里 (实测 ld.lld undefined symbol), 走
 * eglGetProcAddress —— 取不到 NULL 本身就是裁决环节之一 */
typedef EGLImageKHR (*PFN_eglCreateImageKHR_)(EGLDisplay, EGLContext, EGLenum,
                                              EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*PFN_eglDestroyImageKHR_)(EGLDisplay, EGLImageKHR);
typedef void (*PFN_glEGLImageTargetTexture2DOES_)(GLenum, GLeglImageOES);
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <stdio.h>

#define PROBE_TAG "egl-import-probe"
#define PROBE_LOG(...) OH_LOG_INFO(LOG_APP, "[" PROBE_TAG "] " __VA_ARGS__)
#define PROBE_ERR(...) OH_LOG_ERROR(LOG_APP, "[" PROBE_TAG "] " __VA_ARGS__)

/* eglext.h:1441-1444 (SDK); 保守在此重定义防头文件版本差 */
#ifndef EGL_NATIVE_BUFFER_OHOS
#define EGL_NATIVE_BUFFER_OHOS 0x34E1
#endif

#define PROBE_W 64
#define PROBE_H 64

/* 标记落点 = files 根 (D10b, 2026-10-06): 原在 .wine/drive_c/ 下，但那是
 * wine prefix，全新安装要等 wineboot 初始化才存在 —— D8 早启后本探针跑在
 * prefix 初始化之前，fopen("w") 落进不存在的目录静默失败 (WriteMarker 只
 * 打日志无重试)，归档缺标记判 FAIL (job-r20261006-005354 实测)。files 根
 * fresh install 即存在，与 prefix 生命周期解耦。不能反过来在探针里建
 * drive_c: WineEnvService 用 drive_c 存在性判「prefix 已初始化」(ets:835)，
 * 预建会骗过首启引导流。 */
static const char *kMarkerPath =
    "/data/storage/el2/base/files/displayroute-egl-import-probe";

static void WriteMarker(const char *verdict)
{
    FILE *f = fopen(kMarkerPath, "w");
    if (f)
    {
        fprintf(f, "%s\n", verdict);
        fclose(f);
    }
    else
    {
        PROBE_ERR("marker write failed: %{public}s", kMarkerPath);
    }
}

/* 失败出口: 记环节枚举 + 标记, 供 smoke 判定与 spec 回填。
 * 日志参数一律 %{public}: 裸 %s/%x 在 hilog 打出来是 <private>
 * (observability.md 的坑表第 3 条, 首轮实测踩到) */
#define FAIL(step)                                                          \
    do                                                                      \
    {                                                                       \
        PROBE_ERR("VERDICT: fail:%{public}s (eglErr=0x%{public}x "           \
                  "glErr=0x%{public}x)",                                     \
                  step, eglGetError(), glGetError());                        \
        WriteMarker("fail:" step);                                          \
        return;                                                             \
    } while (0)

void ohos_egl_import_probe_run(void)
{
    /* 自缓存: 标记已存在 = 已裁决, 不重跑 */
    if (fopen(kMarkerPath, "r"))
    {
        PROBE_LOG("marker exists, skip (delete %{public}s to re-run)", kMarkerPath);
        return;
    }

    PROBE_LOG("start: EGL_NATIVE_BUFFER_OHOS import chain (spec §5 R-ZC ②)");

    /* ── 1. EGL display + 初始化 ── */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        FAIL("eglGetDisplay");
    if (!eglInitialize(dpy, NULL, NULL))
        FAIL("eglInitialize");

    /* ── 2. GLES2 pbuffer 配置 + 上下文 ── */
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLConfig cfg = EGL_NO_CONFIG_KHR;
    EGLint n_cfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n_cfg) || n_cfg < 1)
        FAIL("eglChooseConfig");
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT)
        FAIL("eglCreateContext");
    const EGLint pb_attrs[] = {EGL_WIDTH, PROBE_W, EGL_HEIGHT, PROBE_H,
                               EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    if (surf == EGL_NO_SURFACE)
        FAIL("eglCreatePbufferSurface");
    if (!eglMakeCurrent(dpy, surf, surf, ctx))
        FAIL("eglMakeCurrent");

    /* ── 3. OH_NativeBuffer 分配 + 写测试图案 ── */
    /* usage 带 HW 位: T4 gles2 场景 buffer 要被 GPU 当纹理采样
     * (native_buffer.h 的 usage 枚举无 MEM_SHARE, 以头文件为准) */
    OH_NativeBuffer_Config nbc = {
        .width = PROBE_W,
        .height = PROBE_H,
        .format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
        .usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                 NATIVEBUFFER_USAGE_CPU_READ_OFTEN |
                 NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE,
        .stride = 0,
    };
    OH_NativeBuffer *nb = OH_NativeBuffer_Alloc(&nbc);
    if (!nb)
        FAIL("OH_NativeBuffer_Alloc");
    void *vir = NULL;
    if (OH_NativeBuffer_Map(nb, &vir) != 0 || !vir)
        FAIL("OH_NativeBuffer_Map");
    int stride = 0;
    {
        OH_NativeBuffer_Config got = nbc;
        OH_NativeBuffer_GetConfig(nb, &got);
        stride = got.stride > 0 ? got.stride : PROBE_W * 4;
    }
    /* 图案: (x+y) 梯度, 首像素哨兵值 0xA1B2C3D4 (RGBA 字节序见回读比对) */
    unsigned char *px = (unsigned char *)vir;
    for (int y = 0; y < PROBE_H; ++y)
        for (int x = 0; x < PROBE_W; ++x)
        {
            unsigned char *p = px + (size_t)y * stride + (size_t)x * 4;
            p[0] = (unsigned char)(x * 4);
            p[1] = (unsigned char)(y * 4);
            p[2] = 0x40;
            p[3] = 0xFF;
        }
    OH_NativeBuffer_Unmap(nb);

    /* ── 4. 导入: 探针的核心裁决点 ── */
    PFN_eglCreateImageKHR_ p_create_img =
        (PFN_eglCreateImageKHR_)eglGetProcAddress("eglCreateImageKHR");
    PFN_eglDestroyImageKHR_ p_destroy_img =
        (PFN_eglDestroyImageKHR_)eglGetProcAddress("eglDestroyImageKHR");
    PFN_glEGLImageTargetTexture2DOES_ p_img_tex =
        (PFN_glEGLImageTargetTexture2DOES_)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    if (!p_create_img || !p_img_tex)
        FAIL("eglGetProcAddress(import entry points)");

    /* 载荷类型二选一 (探针第一轮实测: OH_NativeBuffer* 直传被 EGL 实现
     * 按 native_window 对象校验拒绝 —— hilog "NativeObject Invalid magic
     * illegal"; 备选 = 经 CreateNativeWindowBufferFromNativeBuffer 包成
     * OHNativeWindowBuffer* 再传)。两个都试, 哪个通走哪个, 结论枚举区分。 */
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    OHNativeWindowBuffer *wb = NULL; /* 4b 路径的包装层, 清理需配对销毁 */
    const char *payload_kind = NULL;
    img = p_create_img(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                       (EGLClientBuffer)nb, NULL);
    if (img != EGL_NO_IMAGE_KHR)
    {
        payload_kind = "pass:payload=OH_NativeBuffer";
    }
    else
    {
        PROBE_LOG("payload=OH_NativeBuffer rejected (eglErr=0x%{public}x), "
                  "trying OHNativeWindowBuffer wrapper",
                  eglGetError());
        wb = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(nb);
        if (!wb)
            FAIL("CreateNativeWindowBufferFromNativeBuffer");
        img = p_create_img(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                           (EGLClientBuffer)wb, NULL);
        if (img == EGL_NO_IMAGE_KHR)
            FAIL("eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS, both payloads)");
        payload_kind = "pass:payload=OHNativeWindowBuffer";
    }

    /* ── 5. 纹理 + FBO + 回读比对 ── */
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_img_tex(GL_TEXTURE_2D, img);
    if (glGetError() != GL_NO_ERROR)
        FAIL("glEGLImageTargetTexture2DOES");
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        FAIL("glFramebufferTexture2D");
    unsigned char readback[PROBE_W * PROBE_H * 4];
    glReadPixels(0, 0, PROBE_W, PROBE_H, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    if (glGetError() != GL_NO_ERROR)
        FAIL("glReadPixels");

    /* 比对用内嵌期望值 (x*4, y*4, 0xFF), 不引用 px —— Unmap 后指针失效
     * (M0-T8 实测教训)。通道序假设 GL_RGBA: 若真机出现系统性通道错位,
     * 本身就是裁决数据 (gles2 导入分支的格式映射要按它修正)。 */
    int mismatches = 0;
    int first_bad[4] = {-1, -1, -1, -1};
    for (int y = 0; y < PROBE_H; ++y)
        for (int x = 0; x < PROBE_W; ++x)
        {
            const unsigned char *r = readback + ((size_t)y * PROBE_W + x) * 4;
            unsigned char ex0 = (unsigned char)(x * 4);
            unsigned char ex1 = (unsigned char)(y * 4);
            if (r[0] != ex0 || r[1] != ex1 || r[3] != 0xFF)
            {
                if (first_bad[0] < 0)
                {
                    first_bad[0] = x;
                    first_bad[1] = y;
                    first_bad[2] = r[0];
                    first_bad[3] = r[1];
                }
                ++mismatches;
            }
        }
    if (mismatches != 0)
    {
        PROBE_ERR("pixel mismatch count=%{public}d first=(%{public}d,%{public}d) got=(%{public}d,%{public}d)",
                  mismatches, first_bad[0], first_bad[1], first_bad[2],
                  first_bad[3]);
        FAIL("pixel-compare");
    }

    if (mismatches != 0)
    {
        PROBE_ERR("pixel mismatch count=%{public}d (首差异见上)", mismatches);
        FAIL("pixel-compare");
    }

    /* ── 6. 清理 + 结论 ── */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    if (p_destroy_img)
        p_destroy_img(dpy, img);
    if (wb)
        OH_NativeWindow_DestroyNativeWindowBuffer(wb);
    OH_NativeBuffer_Unreference(nb); /* Alloc 的配对 (native_buffer.h:204) */
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);

    PROBE_LOG("VERDICT: %{public}s — import chain usable "
              "(zero-copy gles2 路线可行, R-ZC ② 裁决数据)",
              payload_kind);
    WriteMarker(payload_kind);
}
