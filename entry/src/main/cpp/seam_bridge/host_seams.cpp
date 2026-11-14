// ============================================================================
// host_seams — entry 侧的接缝适配器与注入点 (方案 B1b)
//
// entry.so 在此定义并填充两个弱符号 (libhgs.so 弱引用消费, registry.h):
//   Hgs_zeroCopyBackend — GraphicsBroker 适配器 (ZC 查询/挂载/ready marker)
//   Hgs_hostHooks       — wine_process 适配器 (桌面 shell 进程簿记)
//   方向正确: libhgs 后加载, 加载期可解析 entry 强符号。
//
// SessionControl 反向 (entry 消费 libhgs 实现): libhgs 提供强符号 C 访问器
// Hgs_getSessionControl, entry 调用期 dlsym(RTLD_DEFAULT) 查询 — 方向性
// 教训见 hgs seam/registry.h 头注释 (链接期绑定对本方向不成立)。
// ============================================================================
#include "seam/registry.h"
#include "include/hgs/external_frames.h"

#include "graphics/graphics_broker.h"
#include "proc/wine_process.h"

#include <dlfcn.h>
#include <hilog/log.h>

#undef LOG_TAG
#define LOG_TAG "WL_SEAM"
#include <string>
#include <vector>

namespace {

void* ResolveHgsSymbol(const char* name) {
    if (auto* symbol = dlsym(RTLD_DEFAULT, name)) return symbol;
    // ArkTS may load NAPI modules with RTLD_LOCAL.
    void* handle = dlopen("libhgs.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return nullptr;
    void* symbol = dlsym(handle, name);
    dlclose(handle);
    return symbol;
}

// ── ZeroCopyBackend → GraphicsBroker 转发 ──────────────────────────────────
class BrokerZeroCopyBackend : public hgs::seam::ZeroCopyBackend
{
public:
    bool IsZeroCopyActive() const override
    {
        return winehua::GraphicsBroker::GetInstance().GetState().active ==
               winehua::GraphicsBackend::Virgl;
    }
    bool IsVulkanPresentMode() const override
    {
        return winehua::GraphicsBroker::GetInstance().IsVulkanPresentMode();
    }
    bool QueryZeroCopySurfaces(std::vector<hgs::seam::ZeroCopySurfaceInfo> &surfaces) const override
    {
        std::vector<winehua::ZeroCopySurfaceInfo> in;
        if (!winehua::GraphicsBroker::GetInstance().QueryZeroCopySurfaces(in)) {
            return false;
        }
        // POD 布局契约一致 (registry 契约), 逐字段拷贝
        surfaces.clear();
        surfaces.reserve(in.size());
        for (const auto &e : in) {
            hgs::seam::ZeroCopySurfaceInfo o;
            o.surfaceKey = e.surfaceKey;
            o.clientPid = e.clientPid;
            o.surfaceId = e.surfaceId;
            o.width = e.width;
            o.height = e.height;
            o.serial = e.serial;
            o.attached = e.attached;
            o.vulkan = e.vulkan;
            surfaces.push_back(o);
        }
        return true;
    }
    bool AttachZeroCopyTarget(uint64_t surfaceKey, OHNativeWindow *producerWindow,
                              uint64_t framePeriodNs, bool vulkan) override
    {
        return winehua::GraphicsBroker::GetInstance().AttachZeroCopyTarget(
            surfaceKey, producerWindow, framePeriodNs, vulkan);
    }
    void DetachZeroCopyTarget(uint64_t surfaceKey) override
    {
        winehua::GraphicsBroker::GetInstance().DetachZeroCopyTarget(surfaceKey);
    }
    void SetZeroCopyFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs) override
    {
        winehua::GraphicsBroker::GetInstance().SetZeroCopyFramePeriod(surfaceKey, framePeriodNs);
    }
    void SetZeroCopySurfaceReady(uint64_t surfaceKey, bool ready) override
    {
        winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(surfaceKey, ready);
    }
};

class WineX11FrameProvider final : public hgs::service::ExternalFrameProvider {
public:
    bool Query(std::vector<hgs::service::ExternalFrameTarget>& targets) override {
        std::vector<winehua::ZeroCopySurfaceInfo> surfaces;
        if (!winehua::GraphicsBroker::GetInstance().QueryZeroCopySurfaces(surfaces)) return false;
        targets.clear();
        targets.reserve(surfaces.size());
        for (const auto& surface : surfaces) {
            targets.push_back({surface.surfaceKey, hgs::service::Protocol::X11, surface.surfaceId,
                               surface.clientPid, surface.width, surface.height, surface.attached, surface.vulkan});
        }
        return true;
    }
    bool Bind(const hgs::service::ExternalFrameTarget& target, NativeWindow* producer, uint64_t periodNs) override {
        auto& broker = winehua::GraphicsBroker::GetInstance();
        if (!broker.AttachZeroCopyTarget(target.token, producer, periodNs, target.vulkan)) return false;
        broker.SetZeroCopySurfaceReady(target.token, true);
        return true;
    }
    void Unbind(const hgs::service::ExternalFrameTarget& target) override {
        auto& broker = winehua::GraphicsBroker::GetInstance();
        broker.SetZeroCopySurfaceReady(target.token, false);
        broker.DetachZeroCopyTarget(target.token);
    }
};

// ── HostLifecycleHooks → wine_process 转发 ─────────────────────────────────
class WineHostHooks : public hgs::seam::HostLifecycleHooks
{
public:
    void OnDesktopRootAppeared() override
    {
        MarkDesktopShellProcesses();
    }
    void OnDesktopSessionEnded() override
    {
        MarkDesktopSessionEnded();
    }
};

BrokerZeroCopyBackend g_zcBackend;
WineX11FrameProvider g_x11FrameProvider;
WineHostHooks g_hostHooks;

// ── SessionControl 的 entry 侧访问点 (dlsym 查询 libhgs C 访问器) ───────────
class EntrySessionControl final : public hgs::seam::SessionControl
{
public:
    static hgs::seam::SessionControl &Get()
    {
        static EntrySessionControl instance;
        return instance;
    }

    bool IsDesktopMode() const override { return impl()->IsDesktopMode(); }
    int OutputWidth() const override { return impl()->OutputWidth(); }
    int OutputHeight() const override { return impl()->OutputHeight(); }
    void PromotePendingDesktopRoot() override { impl()->PromotePendingDesktopRoot(); }
    uint32_t GetDesktopRootToplevelId() const override
    {
        return impl()->GetDesktopRootToplevelId();
    }
    void SetDesktopRootRecognitionEnabled(bool on) override
    {
        impl()->SetDesktopRootRecognitionEnabled(on);
    }
    void SetToplevelEventSuppressed(bool on) override
    {
        impl()->SetToplevelEventSuppressed(on);
    }
    void Stop() override { impl()->Stop(); }
    void ResetSessionState() override { impl()->ResetSessionState(); }
    void ResetFirstFrame() override { impl()->ResetFirstFrame(); }
    bool EnsureBridgeForWineLaunch(const std::string &runtimeDir) override
    {
        return impl()->EnsureBridgeForWineLaunch(runtimeDir);
    }
    void AppendWineGamepadEnv(std::vector<std::string> &env) override
    {
        impl()->AppendWineGamepadEnv(env);
    }

private:
    using ResolveFn = hgs::seam::SessionControl *(*)();

    // 两级查询:
    //  1. RTLD_DEFAULT — 全局作用域; OHOS 的 NAPI so 可能以 RTLD_LOCAL 加载,
    //     此时查不到;
    //  2. dlopen("libhgs.so", RTLD_NOLOAD) 取已加载模块句柄再 dlsym —
    //     NOLOAD 不加载 (libhgs 已由 ArkTS import 加载), 只寻址。
    // 失败不缓存 (模块可能稍后才加载), 仅日志一次便于诊断。
    ResolveFn Resolve() const
    {
        if (resolve_ != nullptr) {
            return resolve_;
        }
        void *sym = ResolveHgsSymbol("Hgs_getSessionControl");
        if (sym == nullptr) {
            if (!missLogged_) {
                missLogged_ = true;
                OH_LOG_WARN(LOG_APP, "[SEAM] Hgs_getSessionControl not resolvable yet");
            }
            return nullptr;
        }
        if (!hitLogged_) {
            hitLogged_ = true;
            OH_LOG_INFO(LOG_APP, "[SEAM] Hgs_getSessionControl resolved");
        }
        resolve_ = reinterpret_cast<ResolveFn>(sym);
        return resolve_;
    }

    // libhgs 未加载 (ArkTS 未 import) 时返回 null_ — 调用方语义 =
    // IsDesktopMode false / 输出尺寸 0, 与未装配降级一致
    hgs::seam::SessionControl *impl() const
    {
        ResolveFn fn = Resolve();
        if (fn == nullptr) {
            return const_cast<hgs::seam::NullSessionControl *>(&null_);
        }
        hgs::seam::SessionControl *p = fn();
        return p != nullptr ? p
                            : const_cast<hgs::seam::NullSessionControl *>(&null_);
    }

    mutable ResolveFn resolve_ = nullptr;
    mutable bool missLogged_ = false;
    mutable bool hitLogged_ = false;
    hgs::seam::NullSessionControl null_{};
};

} // namespace

namespace winehua {
bool ConfigureX11FrameProvider(bool enabled) {
    using Register = decltype(&Hgs_setExternalFrameProvider);
    auto set = reinterpret_cast<Register>(ResolveHgsSymbol("Hgs_setExternalFrameProvider"));
    if (enabled) GraphicsBroker::GetInstance();
    const bool configured = set && set(enabled ? &g_x11FrameProvider : nullptr);
    if (enabled && !configured)
        OH_LOG_WARN(LOG_APP, "HGS frame provider registration failed (%{public}s)",
                    set ? "runtime active" : "symbol unavailable");
    return configured;
}
}

// ── 宿主注入变量 (libhgs 经 dlsym 运行时解析) ───────────────────────────────
extern "C" {
hgs::seam::ZeroCopyBackend *Hgs_zeroCopyBackend = &g_zcBackend;
hgs::seam::HostLifecycleHooks *Hgs_hostHooks = &g_hostHooks;
}

// 宿主各调用点 (wine_launch/wine_env/wine_exe/napi_init) 经此访问
namespace hgs {
namespace seam {
SessionControl &HostSessionControl()
{
    return EntrySessionControl::Get();
}
} // namespace seam
} // namespace hgs
