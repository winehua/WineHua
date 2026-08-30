#pragma once

#include "present_target.h"

#include <native_window/external_window.h>

#include <cstdint>
#include <memory>

namespace winehua {

// Venus 呈现目标 (Vulkan present)。实现 PresentTarget 接口 (见 present_target.h):
// 支持 PresentVenus, 其 Present (GL) 为防御性死路径返回 kPresentInvalid。
class VenusSurfaceQueueTarget : public PresentTarget {
public:
    VenusSurfaceQueueTarget();
    ~VenusSurfaceQueueTarget() override;

    VenusSurfaceQueueTarget(const VenusSurfaceQueueTarget&) = delete;
    VenusSurfaceQueueTarget& operator=(const VenusSurfaceQueueTarget&) = delete;

    bool IsVulkan() const override { return true; }

    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs, OHNativeWindow* window,
               bool releaseWindowWithUnreference) override;
    int Detach(uint64_t surfaceKey) override;
    int SetFramePeriod(uint64_t framePeriodNs) override;
    bool HasVulkanDevice() override;
    bool PrepareDeviceRelease(uint32_t contextId, uintptr_t device) override;
    bool FinishDeviceRelease(uint32_t contextId, uintptr_t device,
                             int32_t waitResult) override;
    int Present(GLuint texture, uint32_t width, uint32_t height,
                uint64_t drawable, uint32_t serial,
                uint64_t* nextPresentDeadlineNs) override;
    int PresentVenus(uint32_t contextId,
                     uintptr_t instance,
                     uintptr_t physicalDevice,
                     uintptr_t device,
                     uintptr_t queue,
                     uint64_t image,
                     uint32_t queueFamily,
                     uint32_t width,
                     uint32_t height,
                     uint32_t format,
                     uint32_t layout,
                     uint32_t serial,
                     uint64_t* nextPresentDeadlineNs,
                     void (*releaseQueue)(void*),
                     void* queueSyncData) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace winehua
