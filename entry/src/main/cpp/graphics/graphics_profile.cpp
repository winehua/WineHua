#include "graphics/graphics_profile.h"

#include <utility>
#include <atomic>

namespace winehua {
namespace {

std::atomic<bool> productDirectVerified{false};
std::atomic<bool> productDirectPresentationSupported{true};

void AppendBooleanEnvironment(std::vector<std::string>* environment,
                              std::string_view key, bool enabled)
{
    environment->emplace_back(std::string(key) + (enabled ? "=1" : "=0"));
}

bool SerializeGuestGraphicsEnvironment(
    std::string_view policyName, const GuestGraphicsPolicy& policy,
    D3dBackendKind backend, std::vector<std::string>* environment)
{
    if (!environment) return false;
    environment->clear();
    DxvkRuntimeProfile runtime{};
    if (backend != D3dBackendKind::WineD3dZink &&
        !ResolveDxvkRuntimeProfile(backend, &runtime)) return false;

    environment->emplace_back("WINEHUA_GRAPHICS_PROFILE=" +
                              std::string(policyName));
    environment->emplace_back(std::string("WINEHUA_VULKAN_BACKEND=") +
                              (policy.directVulkan ? "direct" : "venus"));
    AppendBooleanEnvironment(environment, "WINEHUA_VK_PRECISE_MAP",
                             policy.directPreciseMaps);
    AppendBooleanEnvironment(environment, "DXVK_WINEHUA_PRECISE_SHADOW",
                             policy.preciseShadow);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_STRONG_RING_BARRIER",
                             policy.submission ==
                                 GuestSubmissionPolicy::StrongRingBarrier);
    AppendBooleanEnvironment(environment,
                             "VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE",
                             policy.serializeDescriptorUpdates);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_PERF_SUMMARY",
                             policy.perfSummary);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_DIRECT_FENCE_WAIT",
                             policy.directFenceWait);
    AppendBooleanEnvironment(environment, "WINEHUA_DXVK_TRACE_PRESENT_IMAGE",
                             policy.tracePresentImage);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_DEFER_SHMEM_UNREF",
                             policy.deferSharedMemoryUnref);

    AppendBooleanEnvironment(environment, "VN_WINEHUA_REMOTE_MEMORY_SYNC",
                             policy.preciseShadow);
    // Guest and Host ring words live in separate shadow mappings. Coalescing
    // can miss the renderer entering idle between two sub-millisecond submits.
    // Direct Vulkan has no Venus ring and must not inherit this workaround.
    AppendBooleanEnvironment(environment, "VN_WINEHUA_ALWAYS_NOTIFY_RING",
                             policy.preciseShadow && !policy.directVulkan);
    if (runtime.dynamicMappedFlush)
        AppendBooleanEnvironment(environment,
                                 "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED",
                                 policy.preciseShadow);

    std::string vnPerf = "VN_PERF=no_fence_feedback,no_query_feedback";
    if (runtime.disableSemaphoreFeedback || backend == D3dBackendKind::WineD3dZink)
        vnPerf += ",no_semaphore_feedback";
    switch (policy.submission) {
    case GuestSubmissionPolicy::SynchronousSubmit:
        vnPerf += ",no_async_queue_submit";
        break;
    case GuestSubmissionPolicy::Default:
    case GuestSubmissionPolicy::StrongRingBarrier:
        vnPerf += ",no_multi_ring";
        break;
    }
    environment->emplace_back(std::move(vnPerf));
    if (policy.disableGpuUpload)
        environment->emplace_back("VKR_WINEHUA_GPU_UPLOAD=0");
    return true;
}

} // namespace

D3dBackendKind ParseD3dBackend(std::string_view backend)
{
    if (backend == "wined3d") return D3dBackendKind::WineD3d;
    if (backend == "wined3d_zink") return D3dBackendKind::WineD3dZink;
    if (backend == "dxvk_legacy_d3d9") return D3dBackendKind::DxvkLegacyD3d9;
    if (backend == "dxvk_legacy") return D3dBackendKind::DxvkLegacy;
    if (backend == "dxvk_modern_2_6") return D3dBackendKind::DxvkModern26;
    if (backend == "vkd3d_limited_500k") return D3dBackendKind::Vkd3dLimited500k;
    return D3dBackendKind::Unknown;
}

bool IsDxvkBackend(D3dBackendKind backend)
{
    return backend == D3dBackendKind::DxvkLegacy ||
        backend == D3dBackendKind::DxvkLegacyD3d9 ||
        backend == D3dBackendKind::DxvkModern26;
}

bool UsesVenusPresent(D3dBackendKind backend)
{
    return IsDxvkBackend(backend) || backend == D3dBackendKind::WineD3dZink ||
        backend == D3dBackendKind::Vkd3dLimited500k;
}

static bool ResolveVenusProductGraphicsPolicy(D3dBackendKind backend,
                                             ProductGraphicsPolicy* policy)
{
    if (!policy) return false;
    ProductGraphicsPolicy resolved;
    switch (backend) {
    case D3dBackendKind::WineD3d: {
        resolved.route = kProductVirglRoute;
        resolved.host = {resolved.route, "full", "0"};
        break;
    }
    case D3dBackendKind::WineD3dZink:
    case D3dBackendKind::DxvkLegacyD3d9:
    case D3dBackendKind::DxvkLegacy:
    case D3dBackendKind::DxvkModern26: {
        resolved.route = kProductVulkanRoute;
        resolved.host = {resolved.route, "precise-dirty",
                         "inline-gpu-upload-coverage-sort"};
        resolved.guest.preciseShadow = true;
        resolved.guest.submission =
            GuestSubmissionPolicy::StrongRingBarrier;
        break;
    }
    case D3dBackendKind::Vkd3dLimited500k: {
        resolved.route = kProductVulkanRoute;
        resolved.host = {resolved.route, "precise", "0"};
        resolved.guest.preciseShadow = true;
        resolved.guest.submission =
            GuestSubmissionPolicy::StrongRingBarrier;
        resolved.guest.directFenceWait = true;
        break;
    }
    case D3dBackendKind::Unknown:
        return false;
    }
    *policy = resolved;
    return true;
}

void SetProductDirectVulkanVerified(bool verified)
{
    productDirectVerified.store(verified, std::memory_order_release);
}

bool IsProductDirectVulkanVerified()
{
    return productDirectVerified.load(std::memory_order_acquire);
}

void SetProductDirectPresentationSupported(bool supported)
{
    productDirectPresentationSupported.store(supported, std::memory_order_release);
}

bool ResolveProductGraphicsPolicy(D3dBackendKind backend,
                                  ProductGraphicsPolicy* policy)
{
    if (!ResolveVenusProductGraphicsPolicy(backend, policy)) return false;
    // Legacy DXVK's D3D11 upload/readback contract is verified for PE32 and
    // PE64. Modern DXVK/VKD3D have not passed this WOW64 mapping gate.
    if (backend == D3dBackendKind::DxvkLegacy && IsProductDirectVulkanVerified() &&
        productDirectPresentationSupported.load(std::memory_order_acquire)) {
        policy->guest = {};
        policy->guest.directVulkan = true;
        policy->guest.preciseShadow = true;
        policy->guest.directPreciseMaps = true;
        policy->vulkanDesktop = true;
    }
    return true;
}

bool ResolveDxvkRuntimeProfile(D3dBackendKind backend,
                               DxvkRuntimeProfile* profile)
{
    if (!profile) return false;
    switch (backend) {
    case D3dBackendKind::DxvkLegacyD3d9:
    case D3dBackendKind::DxvkLegacy:
        *profile = {
            "legacy", "1.10.3",
            true,  // relaxedFeatureCompatibility
            true,  // commandQueryReset
            true,  // dynamicMappedFlush
            true,  // batchMappedFlush
            // Product DXVK sessions also expose VKD3D-Proton's D3D12
            // companion. Venus semaphore feedback can stall that vtest path
            // even when legacy DXVK supplies d3d11/dxgi.
            true,  // disableSemaphoreFeedback
        };
        return true;
    case D3dBackendKind::DxvkModern26:
    case D3dBackendKind::Vkd3dLimited500k:
        *profile = {
            "modern-2.6", "2.6.2",
            false, // relaxedFeatureCompatibility
            false, // commandQueryReset
            false, // dynamicMappedFlush
            true,  // batchMappedFlush
            true,  // disableSemaphoreFeedback
        };
        return true;
    case D3dBackendKind::WineD3dZink:
    case D3dBackendKind::WineD3d:
    case D3dBackendKind::Unknown:
        return false;
    }
    return false;
}

bool BuildVenusGuestGraphicsEnvironment(
    D3dBackendKind backend, std::vector<std::string>* environment)
{
    if (!environment) return false;
    environment->clear();
    ProductGraphicsPolicy policy;
    if (!ResolveVenusProductGraphicsPolicy(backend, &policy)) return false;
    return SerializeGuestGraphicsEnvironment(policy.route, policy.guest, backend, environment);
}

bool BuildProductGuestGraphicsEnvironment(
    D3dBackendKind backend, std::vector<std::string>* environment)
{
    if (!environment) return false;
    environment->clear();
    ProductGraphicsPolicy policy;
    if (!ResolveProductGraphicsPolicy(backend, &policy)) return false;
    return SerializeGuestGraphicsEnvironment(policy.route, policy.guest,
                                             backend, environment);
}


} // namespace winehua
