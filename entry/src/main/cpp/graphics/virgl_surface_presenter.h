#pragma once

#include "virgl_ipc_protocol.h"

#include <native_window/external_window.h>

#include <cstdint>

namespace winehua {

int AttachVirglSurfaceTarget(uint64_t surfaceKey, uint64_t framePeriodNs,
                             uint32_t flags, OHNativeWindow* window);
int DetachVirglSurfaceTarget(uint64_t surfaceKey);
int SetVirglSurfaceFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs);
int PresentVirglSurface(uint32_t clientPid, uint32_t surfaceId,
                        uint32_t texture, uint32_t width, uint32_t height,
                        uint64_t drawable, uint32_t serial,
                        uint64_t* nextPresentDeadlineNs);
int PresentVenusSurface(uint32_t contextId,
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
                        uint32_t clientPid,
                        uint32_t surfaceId,
                        uint32_t serial,
                        uint64_t* nextPresentDeadlineNs,
                        void (*releaseQueue)(void*),
                        void* queueSyncData);
int PrepareVenusDeviceRelease(uint32_t contextId, uintptr_t device);
int FinishVenusDeviceRelease(uint32_t contextId, uintptr_t device,
                             int32_t waitResult);
virgl_ipc::SurfaceQueryReply QueryVirglSurfaces();
void ResetVirglSurfaces();

} // namespace winehua
