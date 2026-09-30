/*
 * display_guest_frames.cpp — M2-T5 接收侧 (设计见 display_guest_frames.h)
 *
 * 一个 guest 私有呈现窗的生命周期 (Vulkan/Venus 与 OpenGL/virgl 同形, 后者见
 * dlls/winex11.drv/opengl_winehua.c; 两者的 surfaceId 都是 X window id):
 *   guest vkCreateWin32SurfaceKHR / wgl 私有 swap → 私有面 (tag|X window id)
 *   → guest present → 宿主 SurfaceQueuePresenterManager 按 (pid<<32)|id 找目标
 *   → 无目标: 丢弃并等 2.5s (virgl_surface_presenter.cpp)
 *   → 本模块限频查询发现该面 → 建 OH_ConsumerSurface → 生产窗交 broker 绑定
 *   → 宿主投帧进队列 → 本模块每拍取一格挂到同窗 scene 节点 (X 面之上)
 *   → X 窗销毁 → 本模块摘帧 + 解绑 (宿主回到「无目标即丢弃」)
 */

#include "display_guest_frames.h"

#include "ohos_buffer.h"  /* wl_ohos_consumer_buffer_* (WLR_USE_UNSTABLE 在其内部定义) */
#include "ohos_egl_import.h" /* wl_ohos_egl_active: 只有走 OHOS 导入的 gles2 路径才吃队列 buffer */
#include "ohos_output.h"  /* X 窗落点: frame_anchor / xwindow_alive / frame_set / frame_clear */

#include "graphics/graphics_broker.h"

#include <native_buffer/native_buffer.h> /* NATIVEBUFFER_USAGE_* */
#include <native_image/native_image.h>
#include <native_window/external_window.h>

#include <hilog/log.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "guest-frames"

namespace {

using winehua::GraphicsBroker;
using winehua::ZeroCopySurfaceInfo;

/* 宿主侧 present 在无目标时等 kVenusTargetAttachTimeout (2.5s) 后返 EAGAIN
 * 重试, 因此挂接晚一拍对 guest 只是一次重试 —— 没必要每帧扫宿主面表。 */
constexpr uint64_t kAttachPollIntervalNs = 200ull * 1000 * 1000;
/* 生产侧节拍 = 帧时钟的显示周期 (wl_ohos_output_frame_period_ns: VSync 上报值,
 * 兜底时 33ms)。broker 拿它给 guest 做 pacing —— 硬编码 33ms 会把 guest 钳在
 * 30fps (M2 帧率债), 所以周期变化时下面会逐面重发 (SetZeroCopyFramePeriod)。 */
constexpr uint64_t kStatsLogIntervalNs = 2000ull * 1000 * 1000;

uint64_t CurrentFramePeriodNs()
{
    return wl_ohos_output_frame_period_ns();
}

struct Binding {
    uint64_t surfaceKey = 0;    /* 宿主路由键: (clientPid << 32) | surfaceId */
    uint32_t clientPid = 0;
    uint32_t surfaceId = 0;     /* = X window id (私有面低 32 位) */
    uint64_t generation = 0;    /* 挂接那一刻该窗**记录**的身份 (见 ohos_output.h);
                                 * 只比 id 的判据会被同 id 新窗骗过 (§2.7) */
    OH_NativeImage *image = nullptr;
    /* 引用归 OH_NativeImage (AcquireNativeWindow 的引用), 本模块不另持;
     * 宿主侧的引用由 broker 按 kSurfaceNativeObjectReference 自理。 */
    OHNativeWindow *producerWindow = nullptr;
    /* 帧内容行序修正 (挂接时按面类型定死): GL(virgl) 面要纵向翻, Vulkan(venus)
     * 面不翻 —— 依据见 ohos_output.h wl_ohos_output_client_frame_set 注释。 */
    int flipVertical = 0;
    uint64_t frames = 0;
    uint64_t emptyPolls = 0;
};

std::unordered_map<uint32_t, Binding> g_bindings; /* key = X window id */
uint64_t g_lastAttachPollNs = 0;
uint64_t g_lastStatsNs = 0;
uint64_t g_totalFrames = 0;
uint64_t g_destroyedWindows = 0; /* 窗口销毁 → 解绑次数 */
uint64_t g_reusedWindows = 0;    /* 同 id 换记录 (§2.7 竞态) → 解绑次数 */
uint64_t g_staleFrames = 0;      /* 取到帧时记录已失效 → 丢弃次数 */
uint64_t g_orphanFrames = 0;     /* 窗口在册但帧无处可挂 (锚不可用) → 丢弃次数 */

uint64_t NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

/* 解绑一个窗: 摘帧 → 退订宿主目标 → 销毁消费者面。
 * 顺序不可换: 帧节点上还挂着借来的队列 buffer, 面一销毁再归还就是悬垂调用;
 * 退订要在面销毁前完成 (宿主拿到的是这个面的生产窗)。 */
void DropBinding(Binding &b, const char *reason)
{
    wl_ohos_output_client_frame_clear(b.surfaceId);
    if (b.surfaceKey)
    {
        /* ready 先撤再退订 (与 wayland 侧 zc_bridge 的次序一致): guest 下一拍
         * 就不会再往一个正在消失的目标投帧。 */
        GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(b.surfaceKey, false);
        GraphicsBroker::GetInstance().DetachZeroCopyTarget(b.surfaceKey);
    }
    if (b.image)
        OH_NativeImage_Destroy(&b.image);
    b.producerWindow = nullptr;
    OH_LOG_INFO(LOG_APP,
                "[GUEST-FRAMES] detach key=%{public}llu xwin=%{public}u "
                "pid=%{public}u frames=%{public}llu reason=%{public}s",
                static_cast<unsigned long long>(b.surfaceKey), b.surfaceId, b.clientPid,
                static_cast<unsigned long long>(b.frames), reason);
}

void PullFrame(Binding &b)
{
    struct wlr_buffer *frame;
    if (!b.image)
        return;
    /* 销毁即失效的第二道闸 (第一道是本拍开头的 sweep): 窗口已不在册、或该 id
     * 已被**另一条记录**占用 (generation 变了) 的帧一律不挂。 */
    uint64_t gen = 0;
    if (!wl_ohos_output_client_xwindow_generation(b.surfaceId, &gen) || gen != b.generation)
    {
        frame = wl_ohos_consumer_buffer_acquire(b.image);
        if (frame)
        {
            wl_ohos_consumer_buffer_drop(frame);
            ++g_staleFrames;
        }
        return;
    }
    frame = wl_ohos_consumer_buffer_acquire(b.image);
    if (!frame)
    {
        /* 生产者没提交新帧 (或已被取空) —— 常态, 不是错误 */
        ++b.emptyPolls;
        return;
    }
    const bool shown =
        wl_ohos_output_client_frame_set(b.surfaceId, frame, b.flipVertical,
                                        b.surfaceKey) != 0;
    /* 无论上没上屏, 本模块这一份引用都放掉 (wlr_scene.c: 节点自己
     * wlr_buffer_lock 一份, 不消费调用方的引用): 上屏了对象由节点持有、下次
     * set/摘除时析构归还队列; 没上屏 (窗口在册但锚不可用) 就是丢弃 —— 借来的
     * 格子不还 = 队列槽位永久丢失 (T8 路径 A 实测教训)。 */
    wl_ohos_consumer_buffer_drop(frame);
    if (shown)
    {
        ++b.frames;
        ++g_totalFrames;
    }
    else
    {
        ++g_orphanFrames;
    }
}

void TryAttach(const ZeroCopySurfaceInfo &s)
{
    Binding b;
    int x = 0, y = 0, w = 0, h = 0;

    /* 锚可用 = 窗口在册且 X 面已 associate (帧节点要挂在那个位置) */
    if (!wl_ohos_output_client_frame_anchor(s.surfaceId, &x, &y, &w, &h))
        return;
    /* 记下这条窗口**记录**的身份 (不是 id): 之后每拍比对, id 被复用/窗口被
     * 销毁都会让 generation 对不上 (见 sweep 与 PullFrame 的两道闸)。 */
    uint64_t generation = 0;
    if (!wl_ohos_output_client_xwindow_generation(s.surfaceId, &generation))
        return;

    b.surfaceKey = s.surfaceKey;
    b.generation = generation;
    b.clientPid = s.clientPid;
    b.surfaceId = s.surfaceId;
    b.flipVertical = s.vulkan ? 0 : 1;

    b.image = OH_ConsumerSurface_Create();
    if (!b.image)
    {
        OH_LOG_WARN(LOG_APP,
                    "[GUEST-FRAMES] consumer surface create failed key=%{public}llu "
                    "xwin=%{public}u",
                    static_cast<unsigned long long>(b.surfaceKey), b.surfaceId);
        return;
    }
    /* 尺寸/用途以宿主报的源尺寸为准, 缺失时退窗几何 (0 会被拒) */
    OH_ConsumerSurface_SetDefaultSize(b.image,
                                      static_cast<int32_t>(s.width ? s.width : static_cast<uint32_t>(w)),
                                      static_cast<int32_t>(s.height ? s.height : static_cast<uint32_t>(h)));
    OH_ConsumerSurface_SetDefaultUsage(
        b.image, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
    /* 不开 drop 模式: 它只在 UpdateSurfaceImage (纹理消费) 路径生效, 本模块
     * 是 buffer 消费 (Acquire/ReleaseNativeWindowBuffer), 队列由本模块显式
     * 管理 (native_image.h SetDropBufferMode 注释)。 */
    b.producerWindow = OH_NativeImage_AcquireNativeWindow(b.image);
    if (!b.producerWindow ||
        !GraphicsBroker::GetInstance().AttachZeroCopyTarget(b.surfaceKey, b.producerWindow,
                                                           CurrentFramePeriodNs()))
    {
        OH_LOG_WARN(LOG_APP,
                    "[GUEST-FRAMES] attach failed key=%{public}llu xwin=%{public}u "
                    "window=%{public}p",
                    static_cast<unsigned long long>(b.surfaceKey), b.surfaceId,
                    static_cast<void *>(b.producerWindow));
        OH_NativeImage_Destroy(&b.image);
        return;
    }

    OH_LOG_INFO(LOG_APP,
                "[GUEST-FRAMES] attach key=%{public}llu xwin=%{public}u pid=%{public}u "
                "src=%{public}ux%{public}u win=%{public}dx%{public}d@%{public}d,%{public}d "
                "kind=%{public}s flip=%{public}d",
                static_cast<unsigned long long>(b.surfaceKey), b.surfaceId, b.clientPid,
                s.width, s.height, w, h, x, y, s.vulkan ? "vulkan" : "gl", b.flipVertical);
    /* 挂好目标 = 契约里的 ready (host has a target)。X 路线此前**从不发布**
     * ready ⇒ guest 侧探针恒假、每 300 帧一条「host has no present target」的
     * 假告警，而帧其实正在上屏 (review 2026-10-01)。wayland 侧由 zc_bridge
     * 的 Activate/revoke 发布，两条路线语义一致: 宿主有目标即 ready。 */
    GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(b.surfaceKey, true);
    g_bindings.emplace(b.surfaceId, b);
}

void LogStats(uint64_t nowNs)
{
    if (nowNs - g_lastStatsNs < kStatsLogIntervalNs)
        return;
    const uint64_t frames = g_totalFrames;
    const uint64_t destroyed = g_destroyedWindows;
    const uint64_t reused = g_reusedWindows;
    const uint64_t stale = g_staleFrames;
    const uint64_t orphan = g_orphanFrames;
    static uint64_t lastFrames = 0, lastDestroyed = 0, lastReused = 0,
                    lastStale = 0, lastOrphan = 0;
    const bool changed = frames != lastFrames || destroyed != lastDestroyed ||
                         reused != lastReused || stale != lastStale ||
                         orphan != lastOrphan;
    if (!changed && g_bindings.empty())
    {
        g_lastStatsNs = nowNs;
        return;
    }
    lastFrames = frames;
    lastDestroyed = destroyed;
    lastReused = reused;
    lastStale = stale;
    lastOrphan = orphan;
    g_lastStatsNs = nowNs;
    /* 归还侧不变量计数 (known-issues §2.6): rel_synced = 交出去后发生过 GPU
     * 同步; rel_unsynced_presented = 上屏过却没同步点 (**不变量破损, 必须为
     * 0**, 破损时归还路径还会打 ERROR); rel_unrendered = 未上屏即归还 (合法). */
    uint64_t relTotal = 0, relSynced = 0, relViolations = 0, relUnrendered = 0;
    wl_ohos_consumer_handoff_stats(&relTotal, &relSynced, &relViolations,
                                   &relUnrendered);
    OH_LOG_INFO(LOG_APP,
                "[GUEST-FRAMES] stats bindings=%{public}u frames=%{public}llu "
                "destroyed_windows=%{public}llu reused_windows=%{public}llu "
                "stale_frames=%{public}llu "
                "orphan_frames=%{public}llu scene_frames=%{public}u "
                "rel=%{public}llu rel_synced=%{public}llu "
                "rel_unsynced_presented=%{public}llu rel_unrendered=%{public}llu",
                static_cast<uint32_t>(g_bindings.size()),
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(destroyed),
                static_cast<unsigned long long>(reused),
                static_cast<unsigned long long>(stale),
                static_cast<unsigned long long>(orphan),
                wl_ohos_output_frames_in_scene(),
                static_cast<unsigned long long>(relTotal),
                static_cast<unsigned long long>(relSynced),
                static_cast<unsigned long long>(relViolations),
                static_cast<unsigned long long>(relUnrendered));
}

} // namespace

extern "C" void display_guest_frames_tick(void)
{
    const uint64_t nowNs = NowNs();

    /* 只有 gles2 + OHOS 导入的路径能吃队列 buffer (pixman 回退既不能导入也不能
     * 读 data_ptr): 那种形态下挂接只会白吃队列并刷错误日志, 干脆不接。 */
    if (!wl_ohos_egl_active())
        return;

    /* 1) 销毁即失效: 窗口不在册 **或该 id 已换记录** → 摘帧 + 解绑。
     *    判据是记录身份 (generation) 而不是 id: 同 id 复用窗口这条时序
     *    (§2.7) 下, 只比 id 会让老绑定被新窗"继承" —— 老 guest 的最后一帧
     *    落进新窗且无任何计数。改成按记录判之后, g_reusedWindows 就是 §2.7
     *    那道竞态的真仪器, 命中即判定为失效并留痕。 */
    for (auto it = g_bindings.begin(); it != g_bindings.end();)
    {
        uint64_t gen = 0;
        const bool alive =
            wl_ohos_output_client_xwindow_generation(it->first, &gen) != 0;
        if (!alive || gen != it->second.generation)
        {
            if (alive)
            {
                /* 同 id 新窗: 老绑定按记录身份失效 (这正是 §2.7 要的处置) */
                ++g_reusedWindows;
                OH_LOG_WARN(LOG_APP,
                            "[GUEST-FRAMES] xwindow id reused xwin=%{public}u "
                            "old_gen=%{public}llu new_gen=%{public}llu — binding dropped",
                            it->first,
                            static_cast<unsigned long long>(it->second.generation),
                            static_cast<unsigned long long>(gen));
            }
            DropBinding(it->second, alive ? "xwindow id reused" : "xwindow destroyed");
            ++g_destroyedWindows;
            it = g_bindings.erase(it);
        }
        else
        {
            ++it;
        }
    }

    /* 2) 逐窗取帧 (队列无新帧是常态) */
    for (auto &kv : g_bindings)
        PullFrame(kv.second);

    /* 2b) 显示周期变化 (VSync 接入/停摆、刷新率切换) ⇒ 逐面重发 guest pacing。
     * 不发的话 guest 一直按挂接那一刻的周期生产, 而 presenter 正是按 framePeriodNs
     * 节流 guest 的 present (kPresentThrottled) —— 挂接早于 VSync 上报或刷新率
     * 变化时, 周期就停在旧值。SetZeroCopyFramePeriod 在 broker 侧按已挂接面过滤。 */
    {
        static uint64_t s_periodNs = 0;
        const uint64_t periodNs = CurrentFramePeriodNs();
        if (periodNs != s_periodNs && !g_bindings.empty())
        {
            s_periodNs = periodNs;
            for (auto &kv : g_bindings)
                GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(kv.second.surfaceKey,
                                                                     periodNs);
            OH_LOG_INFO(LOG_APP,
                        "[GUEST-FRAMES] frame period → %{public}llu us (%{public}.1fHz) "
                        "bindings=%{public}llu",
                        (unsigned long long)(periodNs / 1000), 1000000000.0 / (double)periodNs,
                        (unsigned long long)g_bindings.size());
        }
    }

    /* 3) 限频发现新的 guest 面 (只查未绑定的活面)。两类都收:
     *    vulkan=1 → Venus 目标 (guest Vulkan)
     *    vulkan=0 → virgl 目标 (guest OpenGL; winex11 私有 present, 见
     *               dlls/winex11.drv/opengl_winehua.c)
     * 面的归属由下面的锚判定把关 —— 只有 surfaceId 是本路线在册且已 associate
     * 的 X 窗才会被挂接 (锚在 wl_ohos_output_client_frame_anchor 里查), 所以
     * 拿到别的路线的面也不会错挂。 */
    if (nowNs - g_lastAttachPollNs >= kAttachPollIntervalNs)
    {
        g_lastAttachPollNs = nowNs;
        std::vector<ZeroCopySurfaceInfo> surfaces;
        if (GraphicsBroker::GetInstance().QueryZeroCopySurfaces(surfaces))
        {
            for (const auto &s : surfaces)
            {
                if (s.attached || !s.surfaceId)
                    continue;
                /* 跨路线撞车再压一层 (review P4): presenter 报的面没有路线标签,
                 * 唯一把关是"id 恰好等于在册 X 窗号"。X 资源号 = (client << 22) |
                 * local, guest 窗口的 client 号 ≥ 1 ⇒ id ≥ 0x200000; 而 wayland
                 * 的 wl_proxy id 是小整数。低于该段的一律不是 guest 的 X 窗
                 * (X 服务端自有 id 段), 直接跳过, 不去赌撞号。 */
                if (s.surfaceId < 0x200000)
                    continue;
                if (g_bindings.find(s.surfaceId) != g_bindings.end())
                    continue;
                TryAttach(s);
            }
        }
    }

    LogStats(nowNs);
}

extern "C" void display_guest_frames_shutdown(void)
{
    for (auto &kv : g_bindings)
        DropBinding(kv.second, "shutdown");
    g_bindings.clear();
    OH_LOG_INFO(LOG_APP,
                "[GUEST-FRAMES] shutdown frames=%{public}llu destroyed_windows=%{public}llu "
                "reused_windows=%{public}llu stale_frames=%{public}llu "
                "orphan_frames=%{public}llu",
                static_cast<unsigned long long>(g_totalFrames),
                static_cast<unsigned long long>(g_destroyedWindows),
                static_cast<unsigned long long>(g_reusedWindows),
                static_cast<unsigned long long>(g_staleFrames),
                static_cast<unsigned long long>(g_orphanFrames));
}
