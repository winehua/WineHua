#include "virgl_host_config.h"

#include <array>

namespace winehua {
namespace {

bool IsBinaryFlag(const std::string& value)
{
    return value == "0" || value == "1";
}

bool IsSafeField(const std::string& value)
{
    return value.find('|') == std::string::npos &&
        value.find('\n') == std::string::npos &&
        value.find('\r') == std::string::npos;
}

void AppendEnv(std::string& params, const char* key, const std::string& value)
{
    params += "|__env=";
    params += key;
    params += "=";
    params += value;
}

} // namespace

bool ValidateVirglHostConfig(const VirglHostConfig& config, std::string* error)
{
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    const std::array<const std::string*, 11> fields = {
        &config.helperPath, &config.socketPath, &config.libraryPath,
        &config.syncMode, &config.logPath, &config.shadowMode,
        &config.shadowTrace, &config.presentMode,
        &config.shadowMergeRanges, &config.descriptorUpdateSerialize,
        &config.gpuUploadWait,
    };
    for (const std::string* field : fields) {
        if (!field || field->empty() || !IsSafeField(*field))
            return fail("host config contains an empty or unsafe field");
    }
    if (config.helperPath.front() != '/' || config.socketPath.front() != '/' ||
        config.libraryPath.front() != '/' || config.logPath.front() != '/')
        return fail("host config paths must be absolute");
    if (config.syncMode != "egl-thread" && config.syncMode != "egl-main" &&
        config.syncMode != "native-fd")
        return fail("invalid VirGL synchronization mode");
    if (config.presentMode != "fifo" && config.presentMode != "mailbox" &&
        config.presentMode != "fifo-async" && config.presentMode != "fifo-poll")
        return fail("invalid Venus present mode");
    if (!IsBinaryFlag(config.shadowMergeRanges) ||
        !IsBinaryFlag(config.descriptorUpdateSerialize) ||
        !IsBinaryFlag(config.gpuUploadWait))
        return fail("host config flags must be 0 or 1");
    if (error) error->clear();
    return true;
}

uint64_t FingerprintVirglHostConfig(const VirglHostConfig& config)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const std::array<const std::string*, 11> fields = {
        &config.helperPath, &config.socketPath, &config.libraryPath,
        &config.syncMode, &config.logPath, &config.shadowMode,
        &config.shadowTrace, &config.presentMode,
        &config.shadowMergeRanges, &config.descriptorUpdateSerialize,
        &config.gpuUploadWait,
    };
    for (const std::string* field : fields) {
        for (unsigned char byte : *field) {
            hash ^= byte;
            hash *= UINT64_C(1099511628211);
        }
        hash ^= UINT8_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool BuildVirglHostLaunchConfig(const VirglHostConfig& config,
                                VirglHostLaunchConfig* launch,
                                std::string* error)
{
    if (!launch) {
        if (error) *error = "launch config output is null";
        return false;
    }
    if (!ValidateVirglHostConfig(config, error)) return false;

    const bool explicitToHost = config.shadowMode == "to-host-explicit";
    const bool preciseDirty = config.shadowMode == "precise-dirty";
    const bool precise = config.shadowMode == "precise" || preciseDirty;
    const bool bgraArrayTrace =
        config.shadowTrace == "inline-gpu-upload-bgra-array-trace";
    const bool frameAssocTrace =
        config.shadowTrace == "inline-gpu-upload-frame-assoc-trace" ||
        bgraArrayTrace;
    /* Gate C is an isolated diagnostic profile. Keep its mapped-memory and
     * direct-fence semantics while tracing the exact private swapchain image
     * across the guest, vtest and Host presenter boundary. */
    const bool presentImageTrace = config.shadowTrace == "present-image-trace" ||
        config.shadowTrace == "vkd3d-gate-c";
    const bool gpuFrameProfile = config.shadowTrace == "gpu-frame-profile";
    const bool frameTimeline = config.shadowTrace == "frame-timeline";
    const bool sampledPerf =
        config.shadowTrace == "inline-gpu-upload-coverage-sort-sampled";
    const bool captureTrace = config.shadowTrace == "1" || frameAssocTrace;
    const bool forceGpuUpload = config.shadowTrace == "gpu-upload" ||
        config.shadowTrace == "gpu-upload-wait";
    const bool noGpuUploadFast = config.shadowTrace == "no-gpu-upload-fast";
    const bool noGpuUpload = config.shadowTrace == "no-gpu-upload" || noGpuUploadFast;
    const bool serializedGpuUpload =
        config.shadowTrace == "inline-gpu-upload-serialized";
    const bool aliasCover = config.shadowTrace == "inline-gpu-upload-alias-cover";
    const bool coverageSort =
        config.shadowTrace == "inline-gpu-upload-coverage-sort" || aliasCover ||
        gpuFrameProfile || frameTimeline || sampledPerf;
    const bool descriptorSerialized = config.descriptorUpdateSerialize == "1" ||
        config.shadowTrace == "inline-gpu-upload-descriptor-serialized";
    const bool inlineGpuUpload = config.shadowTrace == "inline-gpu-upload" ||
        serializedGpuUpload || coverageSort || descriptorSerialized || frameAssocTrace;
    /* Flush handling and queue submission both mutate the same dirty-range
     * generation. Inline upload is only correct when those operations cannot
     * overlap, including the Vulkan 1.3 QueueSubmit2 path used by modern DXVK. */
    const bool generationSerialized = inlineGpuUpload;
    const bool perfSummary = config.shadowTrace == "perf" ||
        config.shadowTrace == "no-gpu-upload" || descriptorSerialized;
    const bool presentPerfSummary = perfSummary || gpuFrameProfile ||
        frameTimeline || sampledPerf || captureTrace;
    const bool boundBufferList = config.shadowTrace == "perf";
    const bool cpuShadowUpload = config.shadowTrace == "cpu-upload";
    const bool legacyHostSync = config.shadowTrace == "legacy-host-sync";
    const bool gateCTrace = config.shadowTrace == "vkd3d-gate-c";
    const std::string fromHostMode = explicitToHost ? "full" :
        (precise ? "precise" : config.shadowMode);
    const std::string toHostMode = explicitToHost || (precise && !preciseDirty)
        ? "explicit" : "full";
    const std::string sampledTrace = sampledPerf || !precise || frameAssocTrace ? "1" : "0";

    std::string params = config.helperPath + "|" + config.socketPath;
    AppendEnv(params, "LD_LIBRARY_PATH", config.libraryPath);
    AppendEnv(params, "VTEST_USE_GLES", "1");
    AppendEnv(params, "VTEST_USE_EGL_SURFACELESS", "1");
    AppendEnv(params, "VTEST_SYNC_GL_FINISH", "1");
    AppendEnv(params, "WINEHUA_VIRGL_SYNC_MODE", config.syncMode);
    AppendEnv(params, "WINEHUA_VIRGL_LOG_PATH", config.logPath);
    AppendEnv(params, "WINEHUA_VKD3D_GATE_C_TRACE", gateCTrace ? "1" : "0");
    AppendEnv(params, "WINEHUA_VKR_TRACE_SAMPLED", sampledTrace);
    AppendEnv(params, "WINEHUA_VKR_TRACE_CAPTURE", captureTrace ? "1" : "0");
    AppendEnv(params, "WINEHUA_VKR_TRACE_CAPTURE_LIMIT", captureTrace ? "20000" : "512");
    AppendEnv(params, "WINEHUA_RESOURCE_TRACE", captureTrace ? "1" : "0");
    /* Capture traces are diagnostic-only. Include precise mapped-buffer
     * identities so a global-buffer upload can be reconciled with its source
     * range without changing queue or transfer behavior. */
    AppendEnv(params, "WINEHUA_VKR_TRACE_UBO_IDENTITY",
              frameAssocTrace ? "focused" : (captureTrace ? "1" : "0"));
    AppendEnv(params, "WINEHUA_VKR_TRACE_PRESENT_IMAGE", presentImageTrace ? "1" : "0");
    AppendEnv(params, "WINEHUA_VK_PRESENT_TRACE", presentImageTrace ? "1" : "0");
    /* Keep Host source replacement disabled while Gate C validates that the
     * Guest VKD3D submit itself writes the private swapchain image. */
    AppendEnv(params, "WINEHUA_VENUS_FORCE_SOURCE_CLEAR", "0");
    AppendEnv(params, "WINEHUA_VKR_TRACE_PIPELINE",
              (captureTrace || gateCTrace) ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_SHADOW_FROM_HOST", fromHostMode);
    AppendEnv(params, "VKR_WINEHUA_SHADOW_TO_HOST", toHostMode);
    AppendEnv(params, "VKR_WINEHUA_SHADOW_TRACE", captureTrace ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_BGRA_ARRAY_RGBA", bgraArrayTrace ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_PERF_SUMMARY", perfSummary ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_PERF_SAMPLE_INTERVAL", sampledPerf ? "120" : "0");
    AppendEnv(params, "VKR_WINEHUA_FRAME_TIMELINE_INTERVAL", frameTimeline ? "120" : "0");
    AppendEnv(params, "WINEHUA_VENUS_GPU_FRAME_PROFILE",
              (gpuFrameProfile || frameTimeline) ? "1" : "0");
    AppendEnv(params, "WINEHUA_VTEST_PRESENT_PERF_SUMMARY",
              presentPerfSummary ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_GPU_UPLOAD",
              noGpuUpload || cpuShadowUpload || legacyHostSync ? "0" :
              ((forceGpuUpload || inlineGpuUpload || captureTrace) ? "1" : "auto"));
    AppendEnv(params, "VKR_WINEHUA_GPU_UPLOAD_WAIT", config.gpuUploadWait);
    AppendEnv(params, "VKR_WINEHUA_GPU_UPLOAD_INLINE", inlineGpuUpload ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_COVERAGE_SORT", coverageSort ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_GPU_UPLOAD_SERIALIZE",
              serializedGpuUpload ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_SHADOW_GENERATION_SERIALIZE",
              generationSerialized ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE",
              descriptorSerialized ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_SHADOW_DIRTY_LIST", legacyHostSync ? "0" : "1");
    AppendEnv(params, "VKR_WINEHUA_BOUND_BUFFER_LIST", boundBufferList ? "1" : "0");
    AppendEnv(params, "VKR_WINEHUA_BATCH_FLUSH",
              (legacyHostSync || noGpuUploadFast) ? "0" : "1");
    AppendEnv(params, "VKR_WINEHUA_SHADOW_MERGE_RANGES", config.shadowMergeRanges);
    AppendEnv(params, "VKR_WINEHUA_SHADOW_COVER_UPLOAD", aliasCover ? "1" : "0");
    AppendEnv(params, "WINEHUA_VKR_PRESENT_STAGE_TRACE",
              (captureTrace || presentImageTrace) ? "1" : "0");
    AppendEnv(params, "WINEHUA_VKR_PRESENT_PREWAIT",
              frameAssocTrace ? "1" : "0");
    AppendEnv(params, "WINEHUA_VKR_SUBMIT_POSTWAIT",
              frameAssocTrace ? "1" : "0");
    AppendEnv(params, "WINEHUA_VENUS_PRESENT_MODE", config.presentMode);
    AppendEnv(params, "EGL_PLATFORM", "surfaceless");
    if (config.syncMode == "egl-thread")
        AppendEnv(params, "VIRGL_DISABLE_NATIVE_FENCE_FD", "1");

    launch->entryParams = std::move(params);
    launch->fingerprint = FingerprintVirglHostConfig(config);
    /* Gate C memory records are written by the in-process renderer to its
     * private log. Forward only filtered vkd3d-gate-c records so a device
     * run can prove the exact flush/invalidate order through hilog without
     * exposing unrelated application data or requiring sandbox access. */
    launch->forwardPerfSummary = perfSummary || frameTimeline || sampledPerf ||
        gateCTrace;
    if (error) error->clear();
    return true;
}

} // namespace winehua
