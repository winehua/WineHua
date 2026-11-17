#pragma once

#include <cstdlib>
#include <string_view>

namespace winehua {

// Immutable for one attached NativeWindow. Product/LAB configuration changes
// already require a graphics-host restart, so presenters should not re-read a
// collection of environment switches on every frame.
struct PresenterRuntimePolicy {
    bool traceStages = false;
    bool gpuFrameProfile = false;
    bool perfSummary = false;
    bool traceFrameOrder = false;
};

inline bool EnabledPolicyValue(std::string_view value)
{
    return value == "1";
}

inline PresenterRuntimePolicy ResolvePresenterRuntimePolicy(
    std::string_view shadowSelector,
    std::string_view gpuFrameProfile,
    std::string_view perfSummary,
    std::string_view presentImageTrace)
{
    PresenterRuntimePolicy policy;
    policy.traceStages = EnabledPolicyValue(shadowSelector);
    policy.gpuFrameProfile = EnabledPolicyValue(gpuFrameProfile);
    policy.perfSummary = EnabledPolicyValue(perfSummary);
    policy.traceFrameOrder = policy.traceStages ||
        EnabledPolicyValue(presentImageTrace);
    return policy;
}

inline std::string_view PresenterPolicyEnvironmentValue(const char* key)
{
    const char* value = std::getenv(key);
    return value ? std::string_view(value) : std::string_view();
}

inline PresenterRuntimePolicy ReadPresenterRuntimePolicyFromEnvironment()
{
    return ResolvePresenterRuntimePolicy(
        PresenterPolicyEnvironmentValue("VKR_WINEHUA_SHADOW_TRACE"),
        PresenterPolicyEnvironmentValue("WINEHUA_VENUS_GPU_FRAME_PROFILE"),
        PresenterPolicyEnvironmentValue("WINEHUA_VTEST_PRESENT_PERF_SUMMARY"),
        PresenterPolicyEnvironmentValue("WINEHUA_VKR_TRACE_PRESENT_IMAGE"));
}

} // namespace winehua
