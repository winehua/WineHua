#include "direct_desktop_compositor.h"
#include "direct_wine_surface_controller.h"
#include "compositor/toplevel/desktop_compositor.h"
#include "graphics/shader_utils.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "DirectDesktop"

namespace winehua::direct {
namespace {

using Rect = ZeroCopyOccluderRect;

// Subtract upper opaque window/menu rectangles from a Direct layer. Draw
// scissored pieces of the original viewport, preserving its texture mapping.
std::vector<Rect> VisibleRects(Rect base, const std::vector<Rect>& occluders)
{
    std::vector<Rect> rects{base};
    for (const auto& cut : occluders) {
        if (cut.w <= 0 || cut.h <= 0) continue;
        std::vector<Rect> next;
        for (const auto& r : rects) {
            int l = std::max(r.x, cut.x), t = std::max(r.y, cut.y);
            int right = std::min(r.x + r.w, cut.x + cut.w);
            int bottom = std::min(r.y + r.h, cut.y + cut.h);
            if (right <= l || bottom <= t) { next.push_back(r); continue; }
            if (t > r.y) next.push_back({r.x, r.y, r.w, t - r.y});
            if (bottom < r.y + r.h) next.push_back({r.x, bottom, r.w, r.y + r.h - bottom});
            if (l > r.x) next.push_back({r.x, t, l - r.x, bottom - t});
            if (right < r.x + r.w) next.push_back({right, t, r.x + r.w - right, bottom - t});
        }
        rects = std::move(next);
    }
    return rects;
}

bool SameLayout(const DirectDesktopLayout& a, const DirectDesktopLayout& b)
{
    if (a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h ||
        a.z != b.z || a.fullscreen != b.fullscreen || a.occluders.size() != b.occluders.size())
        return false;
    for (size_t i = 0; i < a.occluders.size(); ++i) {
        const auto& x = a.occluders[i]; const auto& y = b.occluders[i];
        if (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h) return false;
    }
    return true;
}

} // namespace

struct DirectDesktopCompositor::Impl {
    struct Image {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        OHNativeWindowBuffer* buffer = nullptr;
    };
    struct Layer {
        DirectDesktopSource source;
        std::unordered_map<uint32_t, Image> cache;
        OHNativeWindowBuffer* current = nullptr;
        GLuint texture = 0;
        int width = 0, height = 0;
        bool visible = false;
        DirectDesktopLayout layout;
        uint64_t frames = 0, imports = 0, reuses = 0, draws = 0;
        uint64_t acquireWaits = 0, releaseFences = 0;
    };

    DesktopCompositor& compositor;
    EGLDisplay display;
    bool initialized = false, available = false;
    GLuint program = 0, vbo = 0;
    std::unordered_map<uint32_t, Layer> layers;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
    PFNEGLWAITSYNCKHRPROC waitSync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence = nullptr;

    Impl(DesktopCompositor& c, EGLDisplay d) : compositor(c), display(d) {}

    bool Initialize()
    {
        if (initialized) return available;
        initialized = true;
        const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
        OH_LOG_INFO(LOG_APP, "EGL desktop extensions=%{public}s", extensions ? extensions : "null");
        // The system EGL wrapper handles EGL_NATIVE_BUFFER_OHOS even on
        // drivers whose extension string omits that platform token. Validate
        // the actual import below; synchronization must still be advertised.
        if (!extensions || !std::strstr(extensions, "EGL_ANDROID_native_fence_sync") ||
            !std::strstr(extensions, "EGL_KHR_wait_sync")) {
            OH_LOG_ERROR(LOG_APP, "GPU import/sync extensions missing; Direct desktop unavailable");
            return false;
        }
#define LOAD(member, type, name) member = reinterpret_cast<type>(eglGetProcAddress(name))
        LOAD(createImage, PFNEGLCREATEIMAGEKHRPROC, "eglCreateImageKHR");
        LOAD(destroyImage, PFNEGLDESTROYIMAGEKHRPROC, "eglDestroyImageKHR");
        LOAD(imageTarget, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, "glEGLImageTargetTexture2DOES");
        LOAD(createSync, PFNEGLCREATESYNCKHRPROC, "eglCreateSyncKHR");
        LOAD(destroySync, PFNEGLDESTROYSYNCKHRPROC, "eglDestroySyncKHR");
        LOAD(waitSync, PFNEGLWAITSYNCKHRPROC, "eglWaitSyncKHR");
        LOAD(dupFence, PFNEGLDUPNATIVEFENCEFDANDROIDPROC, "eglDupNativeFenceFDANDROID");
#undef LOAD
        if (!createImage || !destroyImage || !imageTarget || !createSync ||
            !destroySync || !waitSync || !dupFence) return false;
        static const char* fragment = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = vec4(texture(uTex, vUV).rgb, 1.0); }
)";
        GLuint vs = CompileShader(GL_VERTEX_SHADER, kFullscreenQuadVS);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
        program = glCreateProgram();
        glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) return false;
        const float quad[] = {-1,-1,0,1, 1,-1,1,1, -1,1,0,0,
                               1,-1,1,1, 1,1,1,0, -1,1,0,0};
        glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        available = true;
        SetDirectDesktopConsumer(this, true);
        OH_LOG_INFO(LOG_APP, "GPU NativeBuffer desktop consumer enabled; cpuReadBytes=0 cpuUploadBytes=0");
        return true;
    }

    bool WaitAcquire(int& fd)
    {
        if (fd < 0) return true;
        const int backup = dup(fd);
        if (backup < 0) return false;
        const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
        EGLSyncKHR sync = createSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
        if (sync == EGL_NO_SYNC_KHR) { close(backup); return false; }
        fd = -1; // EGL owns the imported fd
        bool ok = waitSync(display, sync, 0) == EGL_TRUE;
        destroySync(display, sync);
        if (ok) close(backup);
        else fd = backup; // failed wait: return producer's original readiness fence
        return ok;
    }

    int ExportReleaseFence()
    {
        const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
                                     EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
        EGLSyncKHR sync = createSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
        int fd = -1;
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush(); fd = dupFence(display, sync); destroySync(display, sync);
        }
        if (fd < 0) {
            // Exceptional cleanup only: never release an image still read by GPU.
            OH_LOG_ERROR(LOG_APP, "release fence export failed; draining GPU for cleanup");
            glFinish();
        }
        return fd;
    }

    void ReleaseCurrent(Layer& layer)
    {
        if (!layer.current) return;
        const int fd = ExportReleaseFence();
        const auto status = layer.source.queue->ReleaseWithStatus(layer.current, fd);
        if (status == DirectImageQueue::ReleaseStatus::Returned) {
            if (fd >= 0) ++layer.releaseFences;
        } else if (status == DirectImageQueue::ReleaseStatus::Error) {
            OH_LOG_ERROR(LOG_APP, "desktop buffer release failed top=%{public}u",
                         layer.source.token.toplevelId);
        } else {
            OH_LOG_INFO(LOG_APP, "desktop buffer retired by producer top=%{public}u", layer.source.token.toplevelId);
        }
        OH_NativeWindow_NativeObjectUnreference(layer.current);
        layer.current = nullptr;
        layer.texture = 0;
    }

    void ClearImages(Layer& layer)
    {
        for (auto& [sequence, image] : layer.cache) {
            glDeleteTextures(1, &image.texture);
            destroyImage(display, image.image);
            OH_NativeWindow_NativeObjectUnreference(image.buffer);
        }
        layer.cache.clear();
    }

    void Retire(Layer& layer)
    {
        ReleaseCurrent(layer);
        // Destruction/resize boundary; never on ordinary frames.
        glFinish();
        ClearImages(layer);
        OH_LOG_INFO(LOG_APP,
            "GPU desktop retire top=%{public}u gen=%{public}llu frames=%{public}llu draws=%{public}llu acquireWaits=%{public}llu releaseFences=%{public}llu",
            layer.source.token.toplevelId, (unsigned long long)layer.source.token.generation,
            (unsigned long long)layer.frames, (unsigned long long)layer.draws,
            (unsigned long long)layer.acquireWaits, (unsigned long long)layer.releaseFences);
    }

    GLuint Import(Layer& layer, OHNativeWindowBuffer* buffer, uint32_t sequence)
    {
        auto found = layer.cache.find(sequence);
        if (found != layer.cache.end()) { ++layer.reuses; return found->second.texture; }
        const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR imported = createImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                                           reinterpret_cast<EGLClientBuffer>(buffer), attributes);
        if (imported == EGL_NO_IMAGE_KHR) {
            OH_LOG_ERROR(LOG_APP, "EGL NativeBuffer import failed top=%{public}u error=0x%{public}x",
                        layer.source.token.toplevelId, eglGetError());
            return 0;
        }
        GLuint texture = 0;
        glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        imageTarget(GL_TEXTURE_2D, imported);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &texture); destroyImage(display, imported); return 0;
        }
        OH_NativeWindow_NativeObjectReference(buffer);
        layer.cache.emplace(sequence, Image{imported, texture, buffer});
        ++layer.imports;
        return texture;
    }

    bool Update()
    {
        if (!Initialize()) return false;
        bool changed = false;
        auto sources = GetDirectDesktopSources(this);
        for (auto it = layers.begin(); it != layers.end();) {
            auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& s) {
                return s.token.toplevelId == it->first && s.queue == it->second.source.queue;
            });
            if (found == sources.end()) {
                Retire(it->second); it = layers.erase(it); changed = true;
            } else ++it;
        }
        for (const auto& source : sources) {
            auto& layer = layers[source.token.toplevelId];
            layer.source = source;
            OHNativeWindowBuffer* buffer = nullptr;
            int fence = -1;
            if (source.queue->Acquire(&buffer, &fence)) {
                OH_NativeWindow_NativeObjectReference(buffer);
                OH_NativeBuffer* native = nullptr;
                OH_NativeBuffer_Config config{};
                if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native) == 0 && native)
                    OH_NativeBuffer_GetConfig(native, &config);
                bool waited = false;
                const bool hasFence = fence >= 0;
                if (config.width > 0 && config.height > 0 && (waited = WaitAcquire(fence))) {
                    if (hasFence) ++layer.acquireWaits;
                    if ((layer.width && (layer.width != config.width || layer.height != config.height)) ||
                        layer.cache.size() >= 8) Retire(layer);
                    GLuint texture = Import(layer, buffer, OH_NativeBuffer_GetSeqNum(native));
                    if (texture) {
                        ReleaseCurrent(layer);
                        layer.current = buffer; layer.texture = texture;
                        layer.width = config.width; layer.height = config.height;
                        buffer = nullptr; ++layer.frames; changed = true;
                    }
                }
                if (buffer) {
                    // An EGL GPU wait has consumed the producer fd. Even an
                    // import failure must return a fence covering that wait.
                    if (waited) fence = ExportReleaseFence();
                    (void)source.queue->Release(buffer, fence); // queue owns the fd
                    OH_NativeWindow_NativeObjectUnreference(buffer);
                    fence = -1;
                }
            }
            if (fence >= 0) close(fence);
            DirectDesktopLayout layout;
            bool visible = layer.current && compositor.GetDirectDesktopLayout(
                source.token.clientPid, source.token.toplevelId, source.token.wlSurfaceId,
                layer.width, layer.height, layout);
            changed |= visible != layer.visible || (visible && !SameLayout(layout, layer.layout));
            layer.visible = visible;
            layer.layout = std::move(layout);
        }
        return changed;
    }

    void Draw(uint32_t rootId, int w, int h, const FitRect& fit)
    {
        if (!available || w <= 0 || h <= 0) return;
        std::vector<Layer*> ordered;
        for (auto& [id, layer] : layers) if (layer.visible) ordered.push_back(&layer);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->layout.z < b->layout.z;
        });
        glUseProgram(program); glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(program, "uTex"), 0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glEnable(GL_SCISSOR_TEST);
        for (auto* layer : ordered) {
            Rect area{layer->layout.x, layer->layout.y, layer->layout.w, layer->layout.h};
            if (layer->layout.fullscreen) {
                FitRect fullscreen;
                if (ComputeFitRect(w, h, area.w, area.h, fullscreen))
                    area = {fullscreen.offX, fullscreen.offY, fullscreen.dstW, fullscreen.dstH};
            }
            const int x = FitMapDisplayX(fit, area.x);
            const int y = FitMapDisplayY(fit, h - area.y - area.h);
            const int width = std::max(1, FitSizeDisplayW(fit, area.w));
            const int height = std::max(1, FitSizeDisplayH(fit, area.h));
            glViewport(x, y, width, height); glBindTexture(GL_TEXTURE_2D, layer->texture);
            Rect clipped{std::max(0, area.x), std::max(0, area.y), 0, 0};
            clipped.w = std::min(w, area.x + area.w) - clipped.x;
            clipped.h = std::min(h, area.y + area.h) - clipped.y;
            if (clipped.w <= 0 || clipped.h <= 0) continue;
            const auto visible = VisibleRects(clipped, layer->layout.occluders);
            if (visible.empty()) continue;
            for (const auto& r : visible) {
                glScissor(FitMapDisplayX(fit, r.x), FitMapDisplayY(fit, h - r.y - r.h),
                          std::max(1, FitSizeDisplayW(fit, r.w)), std::max(1, FitSizeDisplayH(fit, r.h)));
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            ++layer->draws;
            if (layer->draws == 1 || layer->draws % 120 == 0)
                OH_LOG_INFO(LOG_APP,
                    "GPU desktop root=%{public}u top=%{public}u gen=%{public}llu frames=%{public}llu draws=%{public}llu imports=%{public}llu reuses=%{public}llu acquireWaits=%{public}llu releaseFences=%{public}llu image=%{public}dx%{public}d rect=%{public}d,%{public}d+%{public}dx%{public}d occluders=%{public}zu",
                    rootId, layer->source.token.toplevelId, (unsigned long long)layer->source.token.generation,
                    (unsigned long long)layer->frames, (unsigned long long)layer->draws,
                    (unsigned long long)layer->imports, (unsigned long long)layer->reuses,
                    (unsigned long long)layer->acquireWaits, (unsigned long long)layer->releaseFences,
                    layer->width, layer->height, area.x, area.y, area.w, area.h,
                    layer->layout.occluders.size());
        }
        glDisable(GL_SCISSOR_TEST);
    }

    ~Impl()
    {
        for (auto& [id, layer] : layers) Retire(layer);
        SetDirectDesktopConsumer(this, false);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (program) glDeleteProgram(program);
    }
};

DirectDesktopCompositor::DirectDesktopCompositor(DesktopCompositor& c, EGLDisplay d)
    : impl_(std::make_unique<Impl>(c, d)) {}
DirectDesktopCompositor::~DirectDesktopCompositor() = default;
bool DirectDesktopCompositor::Update() { return impl_->Update(); }
void DirectDesktopCompositor::Draw(uint32_t root, int w, int h, const FitRect& fit)
{ impl_->Draw(root, w, h, fit); }

} // namespace winehua::direct
