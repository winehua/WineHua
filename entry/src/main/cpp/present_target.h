#pragma once

#include <GLES3/gl3.h>
#include <native_window/external_window.h>

#include <cerrno>
#include <cstdint>

// ============================================================================
// present_target: 呈现目标统一接口 (重构第 3 步)
//
// 原 SurfaceQueuePresenterManager 用 if (flags & kSurfaceVulkan) 在
// SurfaceQueueTarget (virgl GL blit) 与 VenusSurfaceQueueTarget (Vulkan present)
// 两个具体类之间做事实多态, 且 virgl 的 Present 裸魔数 -2..-7 返回值散落。
// 本接口把两个目标收敛为同名抽象, 管理器只持一个 PresentTarget 指针, 按
// 目标类型 (IsVulkan) 而非硬编码 if/else 调度; 返回码命名化 (数值与旧实现
// 逐点一致, 消费者 virgl_child.cpp 依赖 < -2 且 != -6 的日志门控语义保留)。
//
// 约定: 每个具体实现只支持一种呈现路径 — virgl 实现 Present (GL), 其
// PresentVenus 返回 kPresentInvalid; venus 实现 PresentVenus, 其 Present
// 返回 kPresentInvalid。Manager 已在调度处按 IsVulkan 防错, 这些"不支持"
// 分支为防御性死路径。
// ============================================================================

namespace winehua {

// -- 返回码 (数值与旧实现逐点一致) --
// virgl Present 负值 (原裸魔数): 消费方保留 < -2 且 != -6 的上日志门控语义
constexpr int kPresentFenceSyncFailed = -7;     // glFenceSync 失败
constexpr int kPresentBlitFailed = -6;          // 交换/恢复失败 (掉帧, 常用不漂日志)
constexpr int kPresentMakeCurrentFailed = -5;   // eglMakeCurrent(本 target) 失败
constexpr int kPresentGlSetupFailed = -4;       // EnsureGl 失败
constexpr int kPresentSourceInvisible = -3;     // guest 纹理不可见
constexpr int kPresentNoTarget = -2;            // 目标未绑定/参数非法
// 复用 errno (venus 与管理路径)
constexpr int kPresentInvalid = -EINVAL;        // 错误目标类型或参数非法
// 非负语义
constexpr int kPresentOk = 0;
constexpr int kPresentThrottled = 1;            // 帧间隔节流 (消费方重试, 非失败)

// -- 呈现目标抽象接口 --
class PresentTarget {
public:
    virtual ~PresentTarget() = default;

    virtual int Attach(uint64_t surfaceKey, uint64_t framePeriodNs,
                       OHNativeWindow* window, bool releaseWindowWithUnreference) = 0;
    virtual int Detach(uint64_t surfaceKey) = 0;
    virtual int SetFramePeriod(uint64_t framePeriodNs) = 0;

    // 目标类型: venus 走 Vulkan, virgl 走 GL blit。管理器的调度判定用
    // 它取代硬编码 flags if/else。
    virtual bool IsVulkan() const = 0;

    // GL blit present (virgl 实现)。venus 实现返回 kPresentInvalid。
    virtual int Present(GLuint texture, uint32_t width, uint32_t height,
                        uint64_t drawable, uint32_t serial,
                        uint64_t* nextPresentDeadlineNs) = 0;

    // Vulkan present (venus 实现)。virgl 实现返回 kPresentInvalid。
    virtual int PresentVenus(uint32_t contextId,
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
                             void* queueSyncData) = 0;

    // venus-only device 释放 (无 Vk device 时返回 false; virgl 恒 false)。
    virtual bool HasVulkanDevice() = 0;
    virtual bool PrepareDeviceRelease(uint32_t contextId, uintptr_t device) = 0;
    virtual bool FinishDeviceRelease(uint32_t contextId, uintptr_t device,
                                     int32_t waitResult) = 0;
};

} // namespace winehua
