#pragma once

#include <cstdint>

namespace winehua::virgl_ipc {

constexpr uint32_t kMagic = 0x57484950; // "WHIP"
constexpr int32_t kProtocolVersion = 9;
constexpr uint32_t kMaxSurfaces = 16;
constexpr uint32_t kHostConfigStringCount = 11;

enum RequestCode : uint32_t {
    kConfigureRequest = 1,
    kAttachSurfaceRequest = 2,
    kDetachSurfaceRequest = 3,
    kShutdownRequest = 4,
    kQuerySurfacesRequest = 5,
    kSetFramePeriodRequest = 6,
};

enum SurfaceFlags : uint32_t {
    kSurfaceAttached = 1u << 0,
    kSurfaceVulkan = 1u << 1,
    // Private attach-only flag. The phone process passes an additional native
    // object reference; it must be paired with NativeObjectUnreference rather
    // than the DestroyNativeWindow ownership used by the NCP parcel path.
    kSurfaceNativeObjectReference = 1u << 2,
};

struct SurfaceInfo {
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t surfaceId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;
    uint32_t flags = 0;
};

struct SurfaceQueryReply {
    uint32_t magic = kMagic;
    uint32_t version = static_cast<uint32_t>(kProtocolVersion);
    uint32_t size = sizeof(SurfaceQueryReply);
    uint32_t count = 0;
    SurfaceInfo surfaces[kMaxSurfaces] = {};
};

} // namespace winehua::virgl_ipc
