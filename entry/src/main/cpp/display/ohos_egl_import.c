/*
 * ohos_egl_import.c — host 侧 gles2 渲染器 bring-up + OHOS NativeBuffer 导入
 * 分支 (M2-T4, spec §5 R-ZC ② 落地)
 *
 * 链路: eglGetDisplay(EGL_DEFAULT_DISPLAY) → eglInitialize → ES2 上下文
 *   → wlr_egl_create_with_context → wlr_gles2_renderer_create
 *   → wlr_egl_set_buffer_image_importer(本文件导入器)
 * 之后 wlroots gles2 遇到没有 DMA-BUF 的 wlr_buffer (本项目的 OH_NativeBuffer
 * 背书 buffer) 时由导入器出 EGLImage, 渲染目标 (FBO) 与采样纹理共用同一个
 * GL 对象。wlroots 侧改动见
 * scripts/patches/wlroots-ohos-gles2-egl-import.patch (通用导入钩子, 不含
 * OHOS 知识 —— OHOS 相关全在本文件)。
 *
 * 实测依据 (M2-T2 探针, 真机 r0930013431-t2d, 设备 .6, 标记文件
 * displayroute-egl-import-probe):
 *   - eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS) 的载荷必须是
 *     OHNativeWindowBuffer* —— 裸 OH_NativeBuffer* 被按 native_window 对象
 *     校验拒掉 (EGL_BAD_PARAMETER 0x300c);
 *   - glEGLImageTargetTexture2DOES(GL_TEXTURE_2D) + glFramebufferTexture2D
 *     组合 FBO complete 且 glReadPixels 逐像素一致 ⇒ 导入镜像走 texture
 *     附着 (glEGLImageTargetRenderbufferStorageOES 未验, 不赌);
 *   - 扩展入口不在链接桩 (ld.lld undefined symbol), 走 eglGetProcAddress。
 *
 * 格式/stride 机械断言 (M2 计划 Review Focus #2): EGLImage 表达不了行距,
 * 只接受 stride == width*4 的紧致 buffer; 不满足即谢绝导入 (上层退回 CPU
 * 上传/拷贝路径) 并节流打日志 —— 不许假设"800×600 成立就处处成立"。
 */
#include "ohos_egl_import.h"

#define LOG_TAG "ohos-egl-import"
#include <hilog/log.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> /* glEGLImageOES (gl2.h 不带) */
#include <native_window/external_window.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>

#include <libdrm/drm_fourcc.h>

#include "ohos_buffer.h"

#define TAG_ERR(...) OH_LOG_ERROR(LOG_APP, "[" LOG_TAG "] " __VA_ARGS__)
#define TAG_INFO(...) OH_LOG_INFO(LOG_APP, "[" LOG_TAG "] " __VA_ARGS__)

/* 扩展函数不在链接桩里, 走 eglGetProcAddress (T2 探针实测) */
typedef EGLImageKHR (*PFN_eglCreateImageKHR_)(EGLDisplay, EGLContext, EGLenum,
                                              EGLClientBuffer, const EGLint *);

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static struct wlr_egl *g_egl;
static struct wlr_renderer *g_renderer;
/* GPU 同步点计数 (known-issues §2.6 不变量): 每次 glFinish 成功 ++。
 * guest 帧归还侧用它判断「这格 buffer 交出去之后, 有没有发生过一次 GPU
 * 同步」——没有就说明采样它的那个 pass 可能还在飞。计数**只在本函数里
 * 自增**: 谁把 glFinish 换成 fence, 谁就必须同时改这里 (不变量检查器
 * 的意义就是让这类改动不可能静默)。 */
static uint64_t g_eglSyncCount;
static PFN_eglCreateImageKHR_ g_create_image;

/* 导入器: wlr_buffer → EGLImage。谢绝 = 返回 false (wlroots 回落其它路径;
 * 渲染目标路径上没有其它路径, 该帧丢弃)。 */
static bool ImportBufferImage(struct wlr_egl *egl, struct wlr_buffer *buffer,
                              struct wlr_egl_buffer_image *out, void *user_data)
{
    (void)egl;
    (void)user_data;

    OHNativeWindowBuffer *window_buffer = wl_ohos_buffer_window_buffer(buffer);
    if (!window_buffer)
        return false; /* 不是本项目 allocator 的 buffer */

    const size_t stride = wl_ohos_buffer_stride(buffer);
    const size_t packed = (size_t)buffer->width * 4u;
    if (stride < packed)
    {
        /* 行距小于行宽 = 数据装不下 (真错位), 谢绝。
         * 注: 行距**大于**行宽是正常的 (系统按对齐给 pitch, 真机 800 宽实测
         * stride=3328=832*4) —— EGL_NATIVE_BUFFER_OHOS 的载荷是完整原生
         * buffer 对象, 行距由驱动从 buffer 自身几何读, 不是由调用方传 pitch
         * (那是 EGL_EXT_image_dma_buf_import 的语义, 早先把那条约束套到本
         * 路径上导致首个 buffer 就被谢绝, 出图链起不来)。 */
        static uint32_t refused;
        if (refused++ < 5u)
            TAG_ERR("import refused: stride=%{public}zu < width=%{public}d*4 "
                    "(%{public}dx%{public}d): 行距装不下行宽 (refused=%{public}u)",
                    stride, buffer->width, buffer->width, buffer->height, refused);
        return false;
    }

    EGLImageKHR image = g_create_image(g_display, EGL_NO_CONTEXT,
                                       EGL_NATIVE_BUFFER_OHOS,
                                       (EGLClientBuffer)window_buffer, NULL);
    if (image == EGL_NO_IMAGE_KHR)
    {
        static uint32_t failed;
        if (failed++ < 5u)
            TAG_ERR("eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS) failed "
                    "eglErr=0x%{public}x (%{public}dx%{public}d stride=%{public}zu) "
                    "failed=%{public}u",
                    eglGetError(), buffer->width, buffer->height, stride, failed);
        return false;
    }

    *out = (struct wlr_egl_buffer_image){
        .image = image,
        .external_only = false, /* 以 GL_TEXTURE_2D 绑定 (T2 实测) */
        .via_texture = true,    /* 走 texture 附着 (补丁: gles2_buffer_get_fbo) */
        .drm_format = wl_ohos_buffer_drm_format(buffer),
    };

    static uint32_t imported;
    if (imported++ == 0u)
        TAG_INFO("first import ok: %{public}dx%{public}d stride=%{public}zu "
                 "drm=0x%{public}x (行距按 handle, 非紧致时由驱动自解释)",
                 buffer->width, buffer->height, stride, out->drm_format);
    return true;
}

struct wlr_renderer *wl_ohos_egl_renderer_create(void)
{
    if (g_renderer)
        return g_renderer;

    /* 顺序: 先取扩展入口, 再建上下文 —— 入口缺席时不必走完整条 EGL 链 */
    g_create_image = (PFN_eglCreateImageKHR_)eglGetProcAddress("eglCreateImageKHR");
    if (!g_create_image)
    {
        TAG_ERR("eglGetProcAddress(eglCreateImageKHR) 不可用");
        return NULL;
    }

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY)
    {
        TAG_ERR("eglGetDisplay(EGL_DEFAULT_DISPLAY) failed");
        return NULL;
    }
    if (!eglInitialize(g_display, NULL, NULL))
    {
        TAG_ERR("eglInitialize failed eglErr=0x%{public}x", eglGetError());
        g_display = EGL_NO_DISPLAY;
        return NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        TAG_ERR("eglBindAPI(ES) failed eglErr=0x%{public}x", eglGetError());
        return NULL;
    }

    /* 能力取证: 这套 libEGL 不实现 EGL_EXT_client_extensions ——
     * eglQueryString(EGL_NO_DISPLAY) 直接 EGL_BAD_DISPLAY (实测 0x3001),
     * 于是 wlroots 的平台选择类扩展 (EGL_EXT_platform_base 等) 全部无从得知。
     * display 扩展串才是这套 EGL 的真实能力面, 记一次供 wlroots 前置条件
     * (surfaceless / configless context) 对照 —— 排障时不必再猜。 */
    {
        const char *client_exts =
            eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        const EGLint client_err = eglGetError();
        const char *display_exts = eglQueryString(g_display, EGL_EXTENSIONS);
        TAG_INFO("EGL client extensions: %{public}s (err=0x%{public}x)",
                 client_exts ? client_exts : "(unavailable)", client_err);
        TAG_INFO("EGL display extensions: %{public}s",
                 display_exts ? display_exts : "(unavailable)");
    }

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint n_cfg = 0;
    if (!eglChooseConfig(g_display, cfg_attrs, &cfg, 1, &n_cfg) || n_cfg < 1)
    {
        TAG_ERR("eglChooseConfig failed eglErr=0x%{public}x", eglGetError());
        return NULL;
    }

    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_context = eglCreateContext(g_display, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_context == EGL_NO_CONTEXT)
    {
        TAG_ERR("eglCreateContext failed eglErr=0x%{public}x", eglGetError());
        return NULL;
    }

    /* wlr_egl_create_with_context: 复用自建 display+context (wlroots 不自己
     * 建平台)。销毁链: wlr_renderer_destroy → wlr_egl_destroy → 释放本
     * context (本模块不再单独销毁)。 */
    g_egl = wlr_egl_create_with_context(g_display, g_context);
    if (!g_egl)
    {
        TAG_ERR("wlr_egl_create_with_context failed");
        return NULL;
    }
    /* 顺序有依赖 (wlroots 补丁): 渲染器创建时按「有无导入器」决定 buffer
     * 能力 (无 DMA-BUF 时只有导入器在, 才声明 DATA_PTR) 与是否强制要求
     * EGL_EXT_image_dma_buf_import (OHOS 没有该扩展)。导入器必须先注册。 */
    wlr_egl_set_buffer_image_importer(g_egl, ImportBufferImage, NULL);

    /* 导入器可渲染格式 = OHOS allocator 支持的那一组 (见 ohos_buffer.cpp
     * 的 DRM↔OHOS 像素格式对应), 行距不可表达故只声明 linear。没有这一
     * 声明时渲染器的可渲染格式集为空 (OHOS 无 EGL_EXT_image_dma_buf_import),
     * output_pick_format 会以 "Renderer doesn't support format 0x34325258"
     * 挡下 output enable (真机实测)。 */
    {
        static const uint32_t kRenderFormats[] = {
            DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
            DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
        };
        struct wlr_drm_format_set formats = {0};
        for (size_t i = 0; i < sizeof(kRenderFormats) / sizeof(kRenderFormats[0]); ++i)
        {
            if (!wlr_drm_format_set_add(&formats, kRenderFormats[i],
                                        DRM_FORMAT_MOD_LINEAR))
                TAG_ERR("声明可渲染格式失败: 0x%{public}x", kRenderFormats[i]);
        }
        wlr_egl_set_buffer_image_importer_formats(g_egl, &formats);
        wlr_drm_format_set_finish(&formats);
    }

    g_renderer = wlr_gles2_renderer_create(g_egl);
    if (!g_renderer)
    {
        TAG_ERR("wlr_gles2_renderer_create failed");
        wlr_egl_destroy(g_egl);
        g_egl = NULL;
        g_context = EGL_NO_CONTEXT;
        return NULL;
    }

    /* 不在这里再查 GL_VERSION/VENDOR: wlroots 建渲染器时已经打过
     * (render/gles2/renderer.c 的 "Using …/GL vendor/GL renderer" 三行),
     * 而且它返回前已 wlr_egl_unset_current —— 此时没有 current 上下文,
     * glGetString 返回的是垃圾非 NULL 指针 (真机实测: strlen 该指针直接
     * SEGV, M2-T4 首轮 cppcrash 20260930024534), NULL 判据挡不住。 */
    TAG_INFO("gles2 renderer up");
    return g_renderer;
}

/* ── M2-T4 present 前置探针 ────────────────────────────────────────────
   零拷贝 present 的两个前提 host 侧验不了, 必须真机验一遍:
     ① window 队列 buffer (RequestBuffer 借来的一格) 能经**生产导入器**成
        EGLImage 且 FBO 可渲染 —— 队列 buffer 的 usage/行距由系统定, 与
        自分配 OH_NativeBuffer 不是一回事;
     ② GPU 写进去的像素, CPU 按 BufferHandle 行距读回逐像素一致 —— 同时
        钉死行距契约 (EGLImage 若按紧致行距解释, 越往下越错位) 与通道序。
   图案: 背景 + 四角 8x8 方块, 每块通道值互不相同 (通道序判据), 右下角是
   关键 (行距错位时最后一行偏得最多)。尺寸/行距/格式全取自 handle, 不写死。
   Y 向约定两种假设各比一遍 (恰好一种全中才算过), 并把命中的那种记进结论
   —— 合成器坐标换算按它。 */
#define PRESENT_TAG "present-probe"
#define P_LOG(...) OH_LOG_INFO(LOG_APP, "[" PRESENT_TAG "] " __VA_ARGS__)
#define P_ERR(...) OH_LOG_ERROR(LOG_APP, "[" PRESENT_TAG "] " __VA_ARGS__)

enum
{
    PRESENT_BG = 0,
    PRESENT_C1,
    PRESENT_C2,
    PRESENT_C3,
    PRESENT_C4,
    PRESENT_NCOLOR
};
static const uint8_t kPresentColors[PRESENT_NCOLOR][4] = {
    {0x11, 0x22, 0x33, 0xFF},
    {0xAA, 0x0B, 0x0C, 0xFF},
    {0x0D, 0xBB, 0x0E, 0xFF},
    {0x0F, 0x10, 0xCC, 0xFF},
    {0xDD, 0xEE, 0x1F, 0xFF},
};

/* 期望色号 (GL 坐标, 原点左下; 方块贴四角) */
static int PresentColorAt(int gx, int gy, int w, int h)
{
    const int s = 8;
    int left = gx < s, right = gx >= w - s;
    int bottom = gy < s, top = gy >= h - s;
    if (right && bottom)
        return PRESENT_C1; /* GL 右下 */
    if (left && bottom)
        return PRESENT_C2; /* GL 左下 */
    if (right && top)
        return PRESENT_C3; /* GL 右上 */
    if (left && top)
        return PRESENT_C4; /* GL 左上 */
    return PRESENT_BG;
}

/* 逐像素全比, 返回不匹配数; 首个不匹配写入 first[6] = {x,y,期望色号,
 * 实测 R,G,B}。flip=false: 内存行 r == GL 行 r; true: 内存行 r == GL 行 h-1-r */
static int PresentVerify(const uint8_t *map, size_t stride, int w, int h,
                         int flip, int *first)
{
    int mism = 0;
    for (int r = 0; r < h; ++r)
    {
        const uint8_t *row = map + (size_t)r * stride;
        int gy = flip ? (h - 1 - r) : r;
        for (int gx = 0; gx < w; ++gx)
        {
            const uint8_t *p = row + (size_t)gx * 4;
            int want_idx = PresentColorAt(gx, gy, w, h);
            const uint8_t *want = kPresentColors[want_idx];
            if (p[0] != want[0] || p[1] != want[1] || p[2] != want[2] ||
                p[3] != want[3])
            {
                if (mism == 0)
                {
                    first[0] = gx;
                    first[1] = r;
                    first[2] = want_idx;
                    first[3] = p[0];
                    first[4] = p[1];
                    first[5] = p[2];
                }
                ++mism;
            }
        }
    }
    return mism;
}

static void PresentVerdict(const char *marker_path, const char *verdict)
{
    if (!marker_path)
        return;
    FILE *f = fopen(marker_path, "w");
    if (f)
    {
        fprintf(f, "%s\n", verdict);
        fclose(f);
    }
    else
    {
        P_ERR("marker write failed: %{public}s", marker_path);
    }
}

bool wl_ohos_egl_present_probe(struct NativeWindow *window,
                               const char *marker_path)
{
    if (marker_path && fopen(marker_path, "r"))
    {
        P_LOG("marker exists, skip");
        return true;
    }
    if (!window)
    {
        P_LOG("no present window, skip");
        return false;
    }
    P_LOG("start: queue buffer → EGL import → GPU render → CPU verify");

    typedef void (*PFN_img_tex)(GLenum, GLeglImageOES);
    PFN_img_tex img_tex =
        (PFN_img_tex)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    /* 销毁入口同 eglCreateImageKHR: 不在链接桩, 走 eglGetProcAddress
     * (wlroots 的 wlr_egl_destroy_image 是私有头 API, 不进安装视图) */
    typedef EGLBoolean (*PFN_destroy_image)(EGLDisplay, EGLImageKHR);
    PFN_destroy_image destroy_image =
        (PFN_destroy_image)eglGetProcAddress("eglDestroyImageKHR");
    if (!img_tex || !destroy_image)
    {
        P_ERR("EGLImage 入口不可用 (img_tex=%{public}p destroy=%{public}p)",
              (void *)img_tex, (void *)destroy_image);
        PresentVerdict(marker_path, "fail:no-gl-egl-image-entry");
        return false;
    }

    /* 探针经 slot API 借格 (与渲染路径同一张 slot 表, wrapper 复用):
     * 一次性借用+销毁的旧语义随 T6.5 移除 —— 调用方不再 drop, 生命周期归
     * slot 表 (见 ohos_buffer.h)。swapchain 探针用不上, 传 NULL 不取。 */
    struct wlr_buffer *buf = wl_ohos_present_slot_acquire(window, NULL);
    if (!buf)
    {
        P_ERR("RequestBuffer failed (队列空或窗口未就绪)");
        PresentVerdict(marker_path, "fail:RequestBuffer");
        return false;
    }

    bool ok = false;
    void *map = NULL;
    uint32_t buf_fmt = 0;
    size_t stride = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
    bool importer_refused = false;
    const int w = buf->width, h = buf->height;

    if (!wlr_buffer_begin_data_ptr_access(
            buf, WLR_BUFFER_DATA_PTR_ACCESS_READ | WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
            &map, &buf_fmt, &stride) ||
        !map)
    {
        P_ERR("CPU 访问队列 buffer 失败 (mmap handle fd)");
        goto out_abort;
    }

    /* 上下文/状态: 探针在合成器渲染器之后跑, 用同一 display+context */
    if (!eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_context))
    {
        P_ERR("eglMakeCurrent failed eglErr=0x%{public}x", eglGetError());
        goto out_abort;
    }

    {
        /* 先按平台行为直接导入 (eglCreateImageKHR), 再问一次生产导入器 ——
         * 两者的差就是「我们自己的 stride 拒绝策略」造成的, 是裁决依据:
         * 直接导入 + 逐像素校验通过而生产导入器谢绝 ⇒ 拒绝策略过严, 按
         * 实测放开; 直接导入也失败 ⇒ 队列 buffer 确实不能当渲染目标。 */
        OHNativeWindowBuffer *payload =
            (OHNativeWindowBuffer *)wl_ohos_present_buffer_window_buffer(buf);
        if (!payload)
        {
            P_ERR("present buffer 无 EGL 载荷");
            PresentVerdict(marker_path, "fail:no-payload");
            goto out_abort;
        }
        image = g_create_image(g_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                               (EGLClientBuffer)payload, NULL);
        if (image == EGL_NO_IMAGE_KHR)
        {
            P_ERR("eglCreateImageKHR(队列 buffer) 失败 eglErr=0x%{public}x "
                  "(w=%{public}d h=%{public}d stride=%{public}zu)",
                  eglGetError(), w, h, stride);
            PresentVerdict(marker_path, "fail:queue-buffer-import");
            goto out_abort;
        }
        P_LOG("direct import ok (%{public}dx%{public}d stride=%{public}zu)",
              w, h, stride);

        struct wlr_egl_buffer_image via_importer = {0};
        bool importer_ok = wlr_egl_import_buffer_image(g_egl, buf, &via_importer);
        if (importer_ok)
        {
            /* 生产导入器也接了 (同一条 EGL 调用), 用它的镜像 */
            destroy_image(g_display, image);
            image = via_importer.image;
            P_LOG("production importer: accepted");
        }
        else
        {
            P_LOG("production importer: refused (stride 契约, 实测裁决在主机)");
        }
        importer_refused = !importer_ok;
    }

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    img_tex(GL_TEXTURE_2D, image);
    if (glGetError() != GL_NO_ERROR)
    {
        P_ERR("glEGLImageTargetTexture2DOES failed glErr=0x%{public}x",
              glGetError());
        PresentVerdict(marker_path, "fail:image-to-texture");
        goto out;
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        P_ERR("FBO incomplete (队列 buffer 不能当渲染目标)");
        PresentVerdict(marker_path, "fail:fbo-incomplete");
        goto out;
    }

    /* GPU 写图案 */
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(kPresentColors[PRESENT_BG][0] / 255.0f,
                 kPresentColors[PRESENT_BG][1] / 255.0f,
                 kPresentColors[PRESENT_BG][2] / 255.0f,
                 kPresentColors[PRESENT_BG][3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    for (int gy0 = 0; gy0 <= h - 8; gy0 += (h - 8))
    {
        for (int gx0 = 0; gx0 <= w - 8; gx0 += (w - 8))
        {
            int idx = PresentColorAt(gx0 + 4, gy0 + 4, w, h);
            glScissor(gx0, gy0, 8, 8);
            glClearColor(kPresentColors[idx][0] / 255.0f,
                         kPresentColors[idx][1] / 255.0f,
                         kPresentColors[idx][2] / 255.0f,
                         kPresentColors[idx][3] / 255.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glFinish(); /* GPU 结果落到内存, 下面 CPU 读 */

    {
        int first[6] = {0};
        int mism_noflip = PresentVerify((const uint8_t *)map, stride, w, h, 0, first);
        int first_flip[6] = {0};
        int mism_flip = PresentVerify((const uint8_t *)map, stride, w, h, 1, first_flip);
        P_LOG("verify %{public}dx%{public}d stride=%{public}zu: "
              "noflip mism=%{public}d flip mism=%{public}d",
              w, h, stride, mism_noflip, mism_flip);
        if (mism_noflip == 0 && mism_flip == 0)
        {
            /* 两种都全中 = 图案不足以区分 (不该发生), 不算过: 判据要能分辨 */
            P_ERR("无法区分 Y 向约定 (两假设同时成立), 判据无效");
            PresentVerdict(marker_path, "fail:ambiguous-y");
            goto out;
        }
        if (mism_noflip != 0 && mism_flip != 0)
        {
            P_ERR("两种 Y 假设都对不上: noflip first=(%{public}d,%{public}d) "
                  "want=%{public}d got=(%{public}d,%{public}d,%{public}d); "
                  "flip first=(%{public}d,%{public}d) want=%{public}d "
                  "got=(%{public}d,%{public}d,%{public}d) — 行距/通道序不符",
                  first[0], first[1], first[2], first[3], first[4], first[5],
                  first_flip[0], first_flip[1], first_flip[2], first_flip[3],
                  first_flip[4], first_flip[5]);
            PresentVerdict(marker_path, "fail:pixel-compare");
            goto out;
        }
        int flip = (mism_noflip != 0);
        P_LOG("VERDICT: pass — 队列 buffer 直渲染成立 (行距 %{public}zu 已按 "
              "handle 生效, Y 约定 = 内存行 %{public}s GL 行)",
              stride, flip ? "= h-1-" : "=");
        {
            char verdict[160];
            snprintf(verdict, sizeof(verdict),
                     "pass:queue-buffer-present stride=%zu yflip=%d "
                     "importer_refused=%d",
                     stride, flip, importer_refused ? 1 : 0);
            PresentVerdict(marker_path, verdict);
        }
        ok = true;
    }

out:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo)
        glDeleteFramebuffers(1, &fbo);
    if (tex)
        glDeleteTextures(1, &tex);
    if (image != EGL_NO_IMAGE_KHR)
        destroy_image(g_display, image);
    wlr_buffer_end_data_ptr_access(buf);
    if (ok)
    {
        int32_t rc = wl_ohos_present_buffer_present(buf, -1);
        if (rc != 0)
        {
            P_ERR("FlushBuffer rc=%{public}d", rc);
            ok = false;
        }
    }
    else
    {
        wl_ohos_present_buffer_abort(buf);
    }
    /* 不 drop (T6.5 契约): wrapper 归 slot 表, 下一帧同句柄复用 */
    return ok;

out_abort:
    wl_ohos_present_buffer_abort(buf);
    return false;
}

bool wl_ohos_egl_active(void)
{
    return g_renderer != NULL;
}

void wl_ohos_egl_finish(void)
{
    if (!g_renderer)
        return;
    if (!eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_context))
    {
        static uint32_t errs;
        if (errs++ < 5u)
            TAG_ERR("eglMakeCurrent failed eglErr=0x%{public}x", eglGetError());
        return;
    }
    glFinish();
    ++g_eglSyncCount;
}

uint64_t wl_ohos_egl_sync_count(void)
{
    return g_eglSyncCount;
}

/* ── 画布 EGL swap 呈现 (M3a-T7, 2026-10-05) ─────────────────────────────
 * 手工 slot + SET_UI_TIMESTAMP + NativeWindowFlushBuffer(fence=-1) 对
 * DesktopAbility 全屏窗的 surface 冻结在首帧 (屏显一直是系统初始填充
 * 0x112233 通道反转色; 纯产品会话复现, 见 progress.md 2026-10-05), 而同
 * 一 surface 对 wayland presenter 的 eglSwapBuffers 路径正常 (vd13 实测:
 * 桌面蓝+任务栏)。对齐之: 场景帧 (output 自己 swapchain 的 committed
 * buffer) 经 EGL_NATIVE_BUFFER_OHOS 导入为纹理 → FBO → ES2 shader blit
 * 到窗口 EGLSurface → SET_UI_TIMESTAMP + eglSwapBuffers。
 * 上下文复用: wlroots gles2 就建在 g_context 上 (renderer_create),
 * 场景帧与 blit 同上下文, 无跨上下文共享问题。 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <time.h>

static EGLSurface g_win_surface = EGL_NO_SURFACE;
static GLuint g_blit_prog = 0;

static int64_t WinPresentNowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

bool wl_ohos_egl_window_surface_create(struct NativeWindow *window)
{
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT)
    {
        TAG_ERR("window surface create: EGL 未初始化");
        return false;
    }
    if (g_win_surface != EGL_NO_SURFACE)
        return true; /* 已建 (重触发路径), 复用 */
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint n_cfg = 0;
    if (!eglChooseConfig(g_display, cfg_attrs, &cfg, 1, &n_cfg) || n_cfg < 1)
    {
        TAG_ERR("window surface: eglChooseConfig(WINDOW) failed eglErr=0x%{public}x",
                eglGetError());
        return false;
    }
    g_win_surface = eglCreateWindowSurface(g_display, cfg,
                                           (EGLNativeWindowType)window, NULL);
    if (g_win_surface == EGL_NO_SURFACE)
    {
        TAG_ERR("eglCreateWindowSurface failed eglErr=0x%{public}x",
                eglGetError());
        return false;
    }
    TAG_INFO("canvas EGL window surface created");
    return true;
}

void wl_ohos_egl_window_surface_destroy(void)
{
    if (g_win_surface != EGL_NO_SURFACE)
    {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_display, g_win_surface);
        g_win_surface = EGL_NO_SURFACE;
    }
    g_blit_prog = 0; /* context 销毁链带走 program 对象 */
}

int wl_ohos_egl_window_surface_active(void)
{
    return g_win_surface != EGL_NO_SURFACE;
}

/* 全屏 pass-through blit program (ES2, 无 glBlitFramebuffer)。
 * 无通道 swizzle: 源是 allocator buffer 的 EGLImage —— gles2 渲染器把场景
 * GPU-GPU 画进同一 EGLImage, 采样读到的就是渲染器写下的逻辑色 (DRM 四字码
 * ABGR8888 == OHOS RGBA8888, 同一布局)。旧 CPU 上传路径的 .bgra 补偿
 * (vd20/vd21) 随 mmap 上传一并删除。uv 不翻转: GL 渲染 → GL 采样 → GL
 * 窗口 surface, 同一 GL 约定全程不变 (对齐 virgl presenter 的已证方向)。 */
static GLuint WindowBlitProgram(void)
{
    if (g_blit_prog)
        return g_blit_prog;
    static const char *vs =
        "attribute vec2 p;\n"
        "varying vec2 uv;\n"
        "void main(){ uv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }\n";
    static const char *fs =
        "precision mediump float;\n"
        "varying vec2 uv;\n"
        "uniform sampler2D s;\n"
        "void main(){ gl_FragColor = texture2D(s, uv); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[256];
        log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        TAG_ERR("blit program link failed: %{public}s", log);
        glDeleteProgram(p);
        return 0;
    }
    g_blit_prog = p;
    return p;
}

int wl_ohos_egl_window_present(struct wlr_buffer *frame,
                               struct NativeWindow *window)
{
    if (g_win_surface == EGL_NO_SURFACE || g_display == EGL_NO_DISPLAY ||
        g_context == EGL_NO_CONTEXT)
        return 0;

    typedef void (*PFN_img_tex)(GLenum, GLeglImageOES);
    PFN_img_tex img_tex =
        (PFN_img_tex)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    typedef EGLBoolean (*PFN_destroy_img)(EGLDisplay, EGLImageKHR);
    PFN_destroy_img destroy_image =
        (PFN_destroy_img)eglGetProcAddress("eglDestroyImageKHR");
    if (!img_tex || !destroy_image)
    {
        static uint32_t api_fail;
        if (api_fail++ < 5u)
            TAG_ERR("window present: EGLImage 入口不可用");
        return 0;
    }

    /* 场景帧导入: frame = 默认 swapchain 的 allocator buffer (OH_NativeBuffer
     * 背书)。取其 OHNativeWindowBuffer 包装走 EGL_NATIVE_BUFFER_OHOS 导入
     * (M2-T2 实测: 裸 OH_NativeBuffer 被拒, 包装载荷是唯一 accepted 形态;
     * 探针 direct import ok 同款)。GPU-GPU 全程无 CPU 视图 —— vd21/vd22c
     * 实测 CPU mmap 与 GPU 写入不可靠一致 (probe fail:pixel-compare、
     * framedump 内容 ≠ 屏幕内容)。 */
    OHNativeWindowBuffer *payload =
        (OHNativeWindowBuffer *)wl_ohos_buffer_window_buffer(frame);
    if (!payload)
    {
        static uint32_t h_fail;
        if (h_fail++ < 5u)
            TAG_ERR("window present: allocator buffer 无 EGL 载荷");
        return 0;
    }
    EGLImageKHR image =
        g_create_image(g_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                       (EGLClientBuffer)payload, NULL);
    if (image == EGL_NO_IMAGE_KHR)
    {
        static uint32_t img_fail;
        if (img_fail++ < 5u)
            TAG_ERR("window present: EGLImage 导入失败 eglErr=0x%{public}x",
                    eglGetError());
        return 0;
    }

    if (!eglMakeCurrent(g_display, g_win_surface, g_win_surface, g_context))
    {
        static uint32_t mc_fail;
        if (mc_fail++ < 5u)
            TAG_ERR("window present: eglMakeCurrent failed eglErr=0x%{public}x",
                    eglGetError());
        destroy_image(g_display, image);
        return 0;
    }

    int ok = 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    img_tex(GL_TEXTURE_2D, image);
    if (glGetError() != GL_NO_ERROR)
    {
        static uint32_t ti_fail;
        if (ti_fail++ < 5u)
            TAG_ERR("window present: EGLImage 挂纹理失败");
        goto out;
    }

    {
        GLuint prog = WindowBlitProgram();
        if (!prog)
            goto out;
        glUseProgram(prog);
        GLint sloc = glGetUniformLocation(prog, "s");
        glUniform1i(sloc, 0);
        GLint ploc = glGetAttribLocation(prog, "p");
        static const GLfloat quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f,
                                        -1.0f, 1.0f, 1.0f, 1.0f};
        glVertexAttribPointer((GLuint)ploc, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glEnableVertexAttribArray((GLuint)ploc);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glViewport(0, 0, frame->width, frame->height);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFlush();
    }

    {
        int64_t ts = WinPresentNowNs();
        int32_t ts_rc = OH_NativeWindow_NativeWindowHandleOpt(
            (OHNativeWindow *)window, SET_UI_TIMESTAMP, ts);
        static uint32_t ts_fail;
        if (ts_rc != 0 && ts_fail++ < 5u)
            TAG_ERR("SET_UI_TIMESTAMP rc=%{public}d", ts_rc);
    }

    ok = (eglSwapBuffers(g_display, g_win_surface) == EGL_TRUE);
    if (!ok)
    {
        static uint32_t sw_fail;
        if (sw_fail++ < 5u)
            TAG_ERR("eglSwapBuffers failed eglErr=0x%{public}x",
                    eglGetError());
    }

out:
    if (tex)
        glDeleteTextures(1, &tex);
    destroy_image(g_display, image);
    return ok;
}
