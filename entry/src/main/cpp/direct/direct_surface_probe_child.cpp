#include "surface_probe_protocol.h"

#include <IPCKit/ipc_kit.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_D1_CHILD"
#include <hilog/log.h>

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace {

using winehua::direct::SurfaceProbeFrame;
std::mutex g_finishMutex;
std::condition_variable g_finishCondition;
bool g_finished = false;
OHNativeWindow* g_producerWindow = nullptr;

bool WaitFence(int fd)
{
    if (fd < 0) return true;
    pollfd descriptor{fd, POLLIN, 0};
    int status;
    do {
        status = poll(&descriptor, 1, 5000);
    } while (status < 0 && errno == EINTR);
    close(fd);
    return status > 0 && !(descriptor.revents & (POLLERR | POLLNVAL));
}

SurfaceProbeFrame ProduceFrame(OHNativeWindow* window, int32_t frame,
                               int32_t width, int32_t height)
{
    SurfaceProbeFrame result{};
    result.pid = getpid();
    result.frame = frame;
    result.width = width;
    result.height = height;
    result.stage = winehua::direct::kSurfaceConfigure;
    const uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
    if (OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, width, height) != 0 ||
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT,
                                              NATIVEBUFFER_PIXEL_FMT_RGBA_8888) != 0 ||
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, usage) != 0)
        return result;

    OHNativeWindowBuffer* windowBuffer = nullptr;
    int requestFence = -1;
    result.stage = winehua::direct::kSurfaceRequestBuffer;
    if (OH_NativeWindow_NativeWindowRequestBuffer(window, &windowBuffer, &requestFence) != 0 ||
        !windowBuffer) {
        if (requestFence >= 0) close(requestFence);
        return result;
    }
    OH_NativeWindow_NativeObjectReference(windowBuffer);
    bool flushed = false;
    do {
        result.stage = winehua::direct::kSurfaceRequestFence;
        const bool fenceReady = WaitFence(requestFence);
        requestFence = -1;
        if (!fenceReady) break;
        OH_NativeBuffer* nativeBuffer = nullptr;
        result.stage = winehua::direct::kSurfaceNativeBuffer;
        if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &nativeBuffer) != 0 ||
            !nativeBuffer) break;
        result.bufferSeq = OH_NativeBuffer_GetSeqNum(nativeBuffer);
        OH_NativeBuffer_Config config{};
        OH_NativeBuffer_GetConfig(nativeBuffer, &config);
        result.stage = winehua::direct::kSurfaceBufferConfig;
        if (config.width != width || config.height != height ||
            config.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888 || config.stride < width * 4)
            break;
        void* address = nullptr;
        result.stage = winehua::direct::kSurfaceMap;
        if (OH_NativeBuffer_Map(nativeBuffer, &address) != 0 || !address) break;
        for (int32_t y = 0; y < height; ++y) {
            auto* row = static_cast<uint8_t*>(address) + y * config.stride;
            for (int32_t x = 0; x < width; ++x) {
                auto* pixel = row + x * 4;
                pixel[0] = static_cast<uint8_t>(frame * 31 + 7);
                pixel[1] = 0x5a;
                pixel[2] = 0xa5;
                pixel[3] = 0xff;
            }
        }
        result.stage = winehua::direct::kSurfaceUnmap;
        if (OH_NativeBuffer_Unmap(nativeBuffer) != 0) break;
        Region region{};
        result.stage = winehua::direct::kSurfaceFlush;
        if (OH_NativeWindow_NativeWindowFlushBuffer(window, windowBuffer, -1, region) != 0)
            break;
        flushed = true;
        result.status = 0;
        result.stage = winehua::direct::kSurfaceComplete;
    } while (false);
    if (requestFence >= 0) close(requestFence);
    if (!flushed) OH_NativeWindow_NativeWindowAbortBuffer(window, windowBuffer);
    OH_NativeWindow_NativeObjectUnreference(windowBuffer);
    return result;
}

int OnSurfaceRequest(uint32_t code, const OHIPCParcel* request,
                     OHIPCParcel* reply, void*)
{
    int32_t version = 0;
    if (!request || !reply ||
        OH_IPCParcel_ReadInt32(request, &version) != OH_IPC_SUCCESS ||
        version != winehua::direct::kSurfaceProbeVersion)
        return OH_IPC_CHECK_PARAM_ERROR;
    if (code == winehua::direct::kSurfaceProbeFinish) {
        if (g_producerWindow) {
            OH_NativeWindow_DestroyNativeWindow(g_producerWindow);
            g_producerWindow = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(g_finishMutex);
            g_finished = true;
        }
        g_finishCondition.notify_all();
        return OH_IPCParcel_WriteInt32(reply, 0);
    }
    if (code != winehua::direct::kSurfaceProbeProduce)
        return OH_IPC_CHECK_PARAM_ERROR;
    int32_t frame = -1;
    int32_t width = 0;
    int32_t height = 0;
    if (OH_IPCParcel_ReadInt32(request, &frame) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &width) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadInt32(request, &height) != OH_IPC_SUCCESS ||
        frame < 0 || frame >= winehua::direct::kSurfaceProbeFrameCount ||
        width <= 0 || height <= 0)
        return OH_IPC_CHECK_PARAM_ERROR;
    SurfaceProbeFrame result{};
    result.stage = winehua::direct::kSurfaceReadWindow;
    result.frame = frame;
    if (frame == 0 && !g_producerWindow)
        OH_NativeWindow_ReadFromParcel(const_cast<OHIPCParcel*>(request), &g_producerWindow);
    if (g_producerWindow)
        result = ProduceFrame(g_producerWindow, frame, width, height);
    OH_LOG_INFO(LOG_APP,
        "[DIRECT-D1] frame=%{public}d status=%{public}d stage=%{public}d pid=%{public}d "
        "size=%{public}dx%{public}d seq=%{public}u",
        frame, result.status, result.stage, result.pid,
        width, height, result.bufferSeq);
    return OH_IPCParcel_WriteBuffer(reply,
        reinterpret_cast<const uint8_t*>(&result), sizeof(result));
}

} // namespace

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    return OH_IPCRemoteStub_Create("winehua.direct.D1", OnSurfaceRequest, nullptr, nullptr);
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    std::unique_lock<std::mutex> lock(g_finishMutex);
    g_finishCondition.wait_for(lock, std::chrono::seconds(30), [] { return g_finished; });
    lock.unlock();
    if (g_producerWindow) OH_NativeWindow_DestroyNativeWindow(g_producerWindow);
}
