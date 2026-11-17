#include "graphics/graphics_profile.h"
#include "graphics/present_pacing.h"
#include "graphics/present_policy.h"
#include "graphics/virgl_host_config.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, std::string_view message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void TestBackendPolicy()
{
    using winehua::D3dBackendKind;
    Check(winehua::ParseD3dBackend("wined3d") == D3dBackendKind::WineD3d,
          "WineD3D backend classification");
    Check(winehua::ParseD3dBackend("wined3d_zink") == D3dBackendKind::WineD3dZink,
          "Zink is a distinct session backend");
    Check(winehua::ParseD3dBackend("dxvk_legacy_d3d9") == D3dBackendKind::DxvkLegacyD3d9,
          "native D3D9 is a distinct session backend");
    for (auto backend : {D3dBackendKind::WineD3dZink, D3dBackendKind::DxvkLegacyD3d9}) {
        winehua::ProductGraphicsPolicy policy;
        std::vector<std::string> env;
        Check(winehua::UsesVenusPresent(backend) &&
              winehua::ResolveProductGraphicsPolicy(backend, &policy) &&
              policy.route == winehua::kProductVulkanRoute &&
              policy.guest.preciseShadow && !policy.guest.directVulkan &&
              winehua::BuildProductGuestGraphicsEnvironment(backend, &env),
              "experimental backend retains explicit Venus publication policy");
        Check(std::find(env.begin(), env.end(),
              "VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring") != env.end(),
              "experimental Venus routes disable unshared semaphore feedback");
    }
    Check(winehua::ParseD3dBackend("dxvk_legacy") == D3dBackendKind::DxvkLegacy,
          "DXVK 1.10 backend classification");
    Check(winehua::ParseD3dBackend("dxvk_modern_2_6") == D3dBackendKind::DxvkModern26,
          "DXVK 2.6 backend classification");
    Check(winehua::ParseD3dBackend("typo") == D3dBackendKind::Unknown,
          "unknown backend fails closed");
    Check(winehua::UsesVenusPresent(D3dBackendKind::DxvkLegacy),
          "DXVK 1.10 uses Venus present");
    Check(winehua::UsesVenusPresent(D3dBackendKind::DxvkModern26),
          "DXVK 2.6 uses Venus present");
    Check(!winehua::UsesVenusPresent(D3dBackendKind::WineD3d),
          "WineD3D uses VirGL present");
    winehua::ProductGraphicsPolicy wined3dPolicy;
    winehua::ProductGraphicsPolicy legacyPolicy;
    winehua::ProductGraphicsPolicy modernPolicy;
    winehua::ProductGraphicsPolicy vkd3dPolicy;
    Check(winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::WineD3d, &wined3dPolicy),
          "resolve WineD3D product route");
    Check(wined3dPolicy.route == winehua::kProductVirglRoute &&
              wined3dPolicy.host.shadowMode == "full" &&
              !wined3dPolicy.guest.preciseShadow,
          "WineD3D uses the neutral product VirGL route");
    Check(winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::DxvkLegacy, &legacyPolicy) &&
              winehua::ResolveProductGraphicsPolicy(
                  D3dBackendKind::DxvkModern26, &modernPolicy),
          "resolve both DXVK product routes");
    Check(legacyPolicy.route == winehua::kProductVulkanRoute &&
              modernPolicy.route == winehua::kProductVulkanRoute &&
              legacyPolicy.host.shadowMode == modernPolicy.host.shadowMode &&
              legacyPolicy.host.shadowSelector == modernPolicy.host.shadowSelector,
          "DXVK versions share one product Vulkan policy");
    Check(winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::Vkd3dLimited500k, &vkd3dPolicy),
          "resolve VKD3D product route");
    Check(vkd3dPolicy.route == winehua::kProductVulkanRoute &&
              vkd3dPolicy.host.shadowMode == "precise" &&
              vkd3dPolicy.guest.submission ==
                  winehua::GuestSubmissionPolicy::StrongRingBarrier &&
              vkd3dPolicy.guest.directFenceWait,
          "VKD3D stays on Vulkan route with adapter policy");
    Check(!winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::Unknown, &legacyPolicy),
          "unknown backend has no product route");

    winehua::DxvkRuntimeProfile runtime;
    Check(winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::DxvkLegacy, &runtime),
          "resolve DXVK 1.10 runtime");
    Check(runtime.directory == "legacy" && runtime.version == "1.10.3" &&
              runtime.relaxedFeatureCompatibility &&
              runtime.commandQueryReset && runtime.dynamicMappedFlush &&
              runtime.batchMappedFlush &&
              runtime.disableSemaphoreFeedback,
          "DXVK 1.10 high-performance runtime contract");
    Check(winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::DxvkModern26, &runtime),
          "resolve DXVK 2.6 runtime");
    Check(runtime.directory == "modern-2.6" && runtime.version == "2.6.2" &&
              !runtime.relaxedFeatureCompatibility &&
              !runtime.commandQueryReset && !runtime.dynamicMappedFlush &&
              runtime.batchMappedFlush && runtime.disableSemaphoreFeedback,
          "DXVK 2.6 runtime contract");
    winehua::DxvkRuntimeProfile vkd3dRuntime;
    Check(winehua::ResolveDxvkRuntimeProfile(
              D3dBackendKind::Vkd3dLimited500k, &vkd3dRuntime),
          "resolve VKD3D companion DXGI runtime");
    Check(vkd3dRuntime.directory == runtime.directory &&
              vkd3dRuntime.version == runtime.version &&
              vkd3dRuntime.batchMappedFlush == runtime.batchMappedFlush,
          "VKD3D shares the DXVK 2.6 companion runtime");
    Check(!winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::WineD3d, &runtime),
          "WineD3D does not resolve a DXVK runtime");
    Check(!winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::Unknown, &runtime),
          "unknown backend does not resolve a DXVK runtime");
}

void TestPresentPacing()
{
    winehua::GlPresentFailureBackoff backoff;
    Check(backoff.PendingDeadline(100) == 0, "healthy GL has no retry gate");
    Check(backoff.Fail(100, 8000000) == 8000100, "first GL failure waits one frame");
    Check(backoff.PendingDeadline(101) == 8000100, "failed GL skips GPU before deadline");
    Check(backoff.PendingDeadline(8000100) == 0, "failed GL can retry when due");
    Check(backoff.Fail(8000100, 8000000) == 24000100, "repeated GL failure backs off");
    uint64_t now = 24000100;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t deadline = backoff.Fail(now, 8000000);
        Check(deadline > now && deadline - now <= 50000000,
              "GL failure delay stays future and within Guest clamp");
        now = deadline;
    }
    Check(backoff.Fail(now, 8000000) - now == 50000000, "persistent failure is capped at 20 Hz");
    backoff.Reset();
    Check(backoff.PendingDeadline(now) == 0, "success or new target clears GL retry state");
    Check(backoff.Fail(now, 0) - now == winehua::kDefaultPresentFramePeriodNs,
          "missing GL period cannot disable failure pacing");
    Check(backoff.Fail(UINT64_MAX - 2, 8000000) == UINT64_MAX,
          "GL failure deadline saturates without wraparound");
    Check(winehua::NormalizePresentFramePeriodNs(0) ==
              winehua::kDefaultPresentFramePeriodNs,
          "zero display period uses the common default");
    Check(winehua::NormalizePresentFramePeriodNs(1000000) ==
              winehua::kMinPresentFramePeriodNs,
          "display period clamps to the common minimum");
    Check(winehua::NormalizePresentFramePeriodNs(100000000) ==
              winehua::kMaxPresentFramePeriodNs,
          "display period clamps to the common maximum");
    Check(winehua::PresentPacingPeriodNs(16666667) == 16166667,
          "common pacing applies the dispatch lead");
    Check(winehua::PresentPacingPeriodNs(
              winehua::kMinPresentFramePeriodNs) ==
              winehua::kMinPresentFramePeriodNs,
          "dispatch lead never crosses the common pacing floor");
    const auto first = winehua::EvaluatePresentPacing(100, 0, 16);
    Check(first.presentNow && first.nextDeadlineNs == 0,
          "first frame presents immediately");
    const auto early = winehua::EvaluatePresentPacing(110, 100, 16);
    Check(!early.presentNow && early.nextDeadlineNs == 116,
          "early frame returns the existing deadline");
    const auto due = winehua::EvaluatePresentPacing(116, 100, 16);
    Check(due.presentNow, "frame presents at deadline");
    Check(winehua::RetryPresentDeadlineNs(120, 100, 16) == 136,
          "queue-full retry produces a future deadline");
    Check(winehua::RetryPresentDeadlineNs(110, 100, 16) == 116,
          "queue-full retry preserves a pending deadline");
    Check(winehua::NextPresentDeadlineNs(
              std::numeric_limits<uint64_t>::max() - 4, 16) ==
              std::numeric_limits<uint64_t>::max(),
          "deadline addition saturates");
    Check(!winehua::DirectPresentUsesGuestDeadline(
              winehua::kDirectPresentWarmupFrames - 1),
          "Direct Present warmup keeps clock pacing");
    Check(winehua::DirectPresentUsesGuestDeadline(
              winehua::kDirectPresentWarmupFrames),
          "Direct Present switches to guest deadline after warmup");
    Check(winehua::DirectPresentAcquireTimeoutNs(0) ==
              winehua::kDirectFirstAcquireTimeoutNs,
          "Direct Present allows first-buffer allocation");
    Check(winehua::DirectPresentAcquireTimeoutNs(
              winehua::kDirectPresentWarmupFrames) == 0,
          "Direct Present never blocks queue acquire after warmup");
}

void TestPresenterRuntimePolicy()
{
    const auto production = winehua::ResolvePresenterRuntimePolicy(
        "inline-gpu-upload-coverage-sort", "0", "1", "0");
    Check(!production.traceStages && !production.gpuFrameProfile,
          "production presenter leaves per-frame diagnostics off");
    Check(production.perfSummary,
          "production presenter keeps periodic summary enabled");

    const auto trace = winehua::ResolvePresenterRuntimePolicy(
        "1", "1", "0", "0");
    Check(trace.traceStages && trace.traceFrameOrder && trace.gpuFrameProfile,
          "trace policy derives all requested diagnostics once");
    const auto frameOrder = winehua::ResolvePresenterRuntimePolicy(
        "0", "0", "0", "1");
    Check(frameOrder.traceFrameOrder && !frameOrder.traceStages,
          "present-image trace does not enable all stage logs");
}

void TestVirglHostConfig()
{
    winehua::VirglHostConfig config = {
        "/data/libwinehua_vtest_server.so", "/data/virgl.sock",
        "/data/libs", "egl-thread", "/data/virgl.log", "precise-dirty",
        "perf", "fifo", "1", "0", "0",
    };
    winehua::VirglHostLaunchConfig launch;
    std::string error;
    Check(winehua::BuildVirglHostLaunchConfig(config, &launch, &error),
          "Host config accepts the explicit perf selector");
    Check(launch.entryParams.find(
              "WINEHUA_VTEST_PRESENT_PERF_SUMMARY=1") != std::string::npos &&
              launch.entryParams.find("VKR_WINEHUA_PERF_SUMMARY=1") !=
                  std::string::npos &&
              launch.forwardPerfSummary,
          "Host summary bit reaches renderer, presenter and log forwarder");

    const uint64_t summaryFingerprint =
        winehua::FingerprintVirglHostConfig(config);
    config.shadowTrace = "inline-gpu-upload-coverage-sort";
    Check(winehua::FingerprintVirglHostConfig(config) != summaryFingerprint,
          "summary selector participates in Host configuration identity");
    config.gpuUploadWait = "mailbox";
    Check(!winehua::ValidateVirglHostConfig(config, &error),
          "invalid upload-wait flag fails validation");
    config.gpuUploadWait = "0";
    config.shadowTrace = "inline-gpu-upload-bound-buffers";
    Check(winehua::BuildVirglHostLaunchConfig(config, &launch, &error) &&
              launch.entryParams.find("VKR_WINEHUA_BOUND_BUFFER_LIST=1") != std::string::npos &&
              launch.entryParams.find("VKR_WINEHUA_GPU_UPLOAD_INLINE=1") != std::string::npos &&
              launch.entryParams.find("VKR_WINEHUA_COVERAGE_SORT=1") != std::string::npos &&
              launch.entryParams.find("VKR_WINEHUA_SHADOW_GENERATION_SERIALIZE=1") != std::string::npos &&
              launch.entryParams.find("VKR_WINEHUA_SHADOW_COVER_UPLOAD=0") != std::string::npos &&
              !launch.forwardPerfSummary,
          "bound-buffer isolation preserves upload correctness without enabling diagnostics");
}

} // namespace

static void TestAutomaticDirectPolicy()
{
    using winehua::D3dBackendKind;
    winehua::ProductGraphicsPolicy policy;
    winehua::SetProductDirectVulkanVerified(false);
    Check(winehua::ResolveProductGraphicsPolicy(D3dBackendKind::DxvkLegacy, &policy) &&
          !policy.guest.directVulkan, "unverified capability keeps Venus");
    winehua::SetProductDirectVulkanVerified(true);
    Check(winehua::ResolveProductGraphicsPolicy(D3dBackendKind::DxvkLegacy, &policy) &&
          policy.route == winehua::kProductVulkanRoute && policy.host.name == policy.route &&
          policy.guest.directVulkan && policy.guest.directPreciseMaps && policy.guest.preciseShadow &&
          policy.guest.submission == winehua::GuestSubmissionPolicy::Default,
          "verified product uses paired Direct range copies");
    for (auto backend : {D3dBackendKind::WineD3d, D3dBackendKind::DxvkModern26, D3dBackendKind::Vkd3dLimited500k})
        Check(winehua::ResolveProductGraphicsPolicy(backend, &policy) && !policy.guest.directVulkan,
              "unqualified backends preserve their product path");
    Check(winehua::ResolveProductGraphicsPolicy(D3dBackendKind::DxvkLegacy, &policy) &&
          policy.guest.directVulkan && !policy.guest.tracePresentImage,
          "product policy keeps presentation tracing disabled");
    winehua::SetProductDirectVulkanVerified(false);
    Check(winehua::ResolveProductGraphicsPolicy(D3dBackendKind::DxvkLegacy, &policy) &&
          !policy.guest.directVulkan && !policy.guest.directPreciseMaps,
          "failed verification removes Direct and its mapped-memory protocol");
}

int main()
{
    TestBackendPolicy();
    TestPresentPacing();
    TestPresenterRuntimePolicy();
    TestVirglHostConfig();
    TestAutomaticDirectPolicy();
    if (failures) return EXIT_FAILURE;
    std::cout << "graphics_policy_test: PASS\n";
    return EXIT_SUCCESS;
}
