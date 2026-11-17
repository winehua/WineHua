#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace winehua {

// Normal product operation exposes two routes. Backend-specific compatibility
// stays in the Native policy/adapter and must not create another product
// profile id.
inline constexpr std::string_view kProductVirglRoute = "product-virgl";
inline constexpr std::string_view kProductVulkanRoute = "product-vulkan";

enum class D3dBackendKind {
    Unknown,
    WineD3d,
    WineD3dZink,
    DxvkLegacy,
    DxvkLegacyD3d9,
    DxvkModern26,
    Vkd3dLimited500k,
};

enum class GpuUploadPolicy {
    Automatic,
    Disabled,
    Cpu,
};

struct HostGraphicsProfile {
    std::string_view name;
    std::string_view shadowMode;
    std::string_view shadowSelector;
    bool mergeShadowRanges = true;
    bool waitGpuUpload = false;
    bool serializeDescriptorUpdates = false;
    bool deferSharedMemoryUnref = false;
    bool perfSummary = false;
    GpuUploadPolicy gpuUpload = GpuUploadPolicy::Automatic;
    std::string_view presentMode = "fifo";
};

struct DxvkRuntimeProfile {
    std::string_view directory;
    std::string_view version;
    // Runtime-specific behavior is expressed as narrow capabilities instead
    // of branching on a generation name at every call site.
    bool relaxedFeatureCompatibility = false;
    bool commandQueryReset = false;
    bool dynamicMappedFlush = false;
    bool batchMappedFlush = false;
    bool disableSemaphoreFeedback = false;
};

enum class GuestSubmissionPolicy {
    Default,
    SynchronousSubmit,
    StrongRingBarrier,
};

// Guest-side behavior derived from the same profile that configures the Host.
// Adapters serialize this policy; ArkTS must not rebuild it from profile names.
struct GuestGraphicsPolicy {
    bool preciseShadow = false;
    GuestSubmissionPolicy submission = GuestSubmissionPolicy::Default;
    bool perfSummary = false;
    bool directFenceWait = false;
    bool tracePresentImage = false;
    bool deferSharedMemoryUnref = false;
    bool disableGpuUpload = false;
    bool serializeDescriptorUpdates = false;
    // Direct uses the OHOS driver in the Wine process. This does not imply
    // which graphics API the game calls or which API composes the desktop.
    bool directVulkan = false;
    // Bridge path paired with legacy DXVK's explicit range copies.
    bool directPreciseMaps = false;
};

struct ProductGraphicsPolicy {
    std::string_view route;
    HostGraphicsProfile host;
    GuestGraphicsPolicy guest;
    // A separate consumer decision, resolved centrally with the transport.
    // EGL can consume native Direct images without changing the guest driver.
    bool vulkanDesktop = false;
};

D3dBackendKind ParseD3dBackend(std::string_view backend);
bool IsDxvkBackend(D3dBackendKind backend);
bool UsesVenusPresent(D3dBackendKind backend);
// Resolves one of the two normal product routes. DXVK generations and the
// VKD3D DXGI companion may differ in adapter details without becoming routes.
bool ResolveProductGraphicsPolicy(D3dBackendKind backend,
                                  ProductGraphicsPolicy* policy);
// Set only after the native child buffer/fence roundtrip has passed. Unknown
// or failed capability checks leave product sessions on Venus.
void SetProductDirectVulkanVerified(bool verified);
bool IsProductDirectVulkanVerified();
// Native Wine display capability, separate from the driver/buffer probe.
// X11 uses the private Venus/XID presenter; OHOS Direct has no Xlib WSI.
// Set before resolving Host and Guest, and hold for the complete session.
void SetProductDirectPresentationSupported(bool supported);
// VKD3D uses DXVK 2.6.2's DXGI companion. WineD3D and unknown backends do
// not resolve a DXVK runtime.
bool ResolveDxvkRuntimeProfile(D3dBackendKind backend,
                               DxvkRuntimeProfile* profile);

// Serializes the Guest half of a normal product route. This keeps product
// operation independent from process-level environment overrides.
// Guest Venus contract for a child whose native process cannot use Direct.
// The global Direct capability and the active desktop consumer stay intact.
bool BuildVenusGuestGraphicsEnvironment(
    D3dBackendKind backend, std::vector<std::string>* environment);

bool BuildProductGuestGraphicsEnvironment(
    D3dBackendKind backend, std::vector<std::string>* environment);

} // namespace winehua
