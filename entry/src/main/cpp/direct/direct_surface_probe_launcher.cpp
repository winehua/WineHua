#include "direct_surface_probe_launcher.h"
#include "direct_buffer_import_probe.h"
#include "surface_probe_protocol.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_D1_MAIN"
#include <hilog/log.h>

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <memory>
#include <new>
#include <thread>

namespace winehua::direct {
namespace {

struct SurfaceWork {
    napi_async_work asyncWork = nullptr;
    napi_deferred deferred = nullptr;
    OHIPCRemoteProxy* proxy = nullptr;
    int32_t launchCode = -1;
    int32_t callbackCode = -1;
    int32_t childPid = -1;
    int32_t framesPassed = 0;
    int32_t killCode = -1;
    int32_t width = 0;
    int32_t height = 0;
    int32_t queueSize = 0;
    uint32_t lastBufferSeq = 0;
    bool callbackReceived = false;
    bool pixelCheck = false;
    bool abortMode = false;
    bool gpuMode = false;
    bool importMode = false;
    bool importCheck = false;
    int32_t importVkResult = 0;
    int32_t importCount = 0;
    int32_t reuseCount = 0;
    std::unique_ptr<DirectBufferImportProbe> importer;
    std::atomic<int32_t> frameSignals{0};
    char stage[64] = "pending";
};

void OnFrameAvailable(void* context)
{
    auto* work = static_cast<SurfaceWork*>(context);
    if (work) work->frameSignals.fetch_add(1, std::memory_order_relaxed);
}

std::mutex g_callbackMutex;
std::condition_variable g_callbackCondition;
SurfaceWork* g_activeWork = nullptr;
bool g_callbackPending = false;

int CountOpenFds()
{
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (const dirent* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    closedir(directory);
    return count - 1;
}

int ReadRssKiB()
{
    FILE* statm = std::fopen("/proc/self/statm", "r");
    if (!statm) return -1;
    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    const int scanned = std::fscanf(statm, "%lu %lu", &totalPages, &residentPages);
    std::fclose(statm);
    return scanned == 2 ? static_cast<int>(residentPages * sysconf(_SC_PAGESIZE) / 1024) : -1;
}

void Fail(SurfaceWork& work, const char* stage)
{
    std::snprintf(work.stage, sizeof(work.stage), "%s", stage);
}

void OnChildStarted(int32_t code, OHIPCRemoteProxy* proxy)
{
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        if (g_activeWork) {
            g_activeWork->callbackCode = code;
            g_activeWork->proxy = proxy;
            g_activeWork->callbackReceived = true;
            g_callbackCondition.notify_all();
            return;
        }
        g_callbackPending = false;
    }
    if (proxy) OH_IPCRemoteProxy_Destroy(proxy);
}

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

bool SendFrame(SurfaceWork& work, OHNativeWindow* producer,
               int32_t frame, int32_t width, int32_t height)
{
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    bool valid = request && reply &&
        OH_IPCParcel_WriteInt32(request, kSurfaceProbeVersion) == OH_IPC_SUCCESS &&
        OH_IPCParcel_WriteInt32(request, frame) == OH_IPC_SUCCESS &&
        OH_IPCParcel_WriteInt32(request, width) == OH_IPC_SUCCESS &&
        OH_IPCParcel_WriteInt32(request, height) == OH_IPC_SUCCESS &&
        (frame != 0 || OH_NativeWindow_WriteToParcel(producer, request) == 0);
    if (!valid) {
        Fail(work, "write_window_parcel");
    } else if (OH_IPCRemoteProxy_SendRequest(work.proxy, kSurfaceProbeProduce,
                                              request, reply, nullptr) != OH_IPC_SUCCESS) {
        Fail(work, "produce_ipc");
        valid = false;
    } else {
        const uint8_t* bytes = OH_IPCParcel_ReadBuffer(reply, sizeof(SurfaceProbeFrame));
        if (!bytes) {
            Fail(work, "produce_reply");
            valid = false;
        } else {
            SurfaceProbeFrame result{};
            std::memcpy(&result, bytes, sizeof(result));
            if (result.status != 0 || result.stage != kSurfaceComplete ||
                result.frame != frame || result.width != width || result.height != height ||
                result.pid <= 0 || (work.childPid > 0 && result.pid != work.childPid)) {
                std::snprintf(work.stage, sizeof(work.stage), "child_stage_%d", result.stage);
                valid = false;
            } else {
                work.childPid = result.pid;
                work.lastBufferSeq = result.bufferSeq;
            }
        }
    }
    if (request) OH_IPCParcel_Destroy(request);
    if (reply) OH_IPCParcel_Destroy(reply);
    return valid;
}

bool ConsumeFrame(SurfaceWork& work, OH_NativeImage* image,
                  int32_t frame, int32_t width, int32_t height)
{
    OHNativeWindowBuffer* windowBuffer = nullptr;
    int fence = -1;
    int32_t acquired = -1;
    for (int retry = 0; retry < 100; ++retry) {
        acquired = OH_NativeImage_AcquireNativeWindowBuffer(image, &windowBuffer, &fence);
        if (acquired == 0 && windowBuffer) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (acquired != 0 || !windowBuffer) {
        if (fence >= 0) close(fence);
        std::snprintf(work.stage, sizeof(work.stage), "acquire_%d_signals_%d",
                      acquired, work.frameSignals.load(std::memory_order_relaxed));
        return false;
    }
    OH_NativeWindow_NativeObjectReference(windowBuffer);
    bool valid = false;
    do {
        const bool fenceReady = WaitFence(fence);
        fence = -1;
        if (!fenceReady) {
            Fail(work, "acquire_fence");
            break;
        }
        OH_NativeBuffer* nativeBuffer = nullptr;
        if (OH_NativeBuffer_FromNativeWindowBuffer(windowBuffer, &nativeBuffer) != 0 ||
            !nativeBuffer) {
            Fail(work, "consumer_native_buffer");
            break;
        }
        OH_NativeBuffer_Config config{};
        OH_NativeBuffer_GetConfig(nativeBuffer, &config);
        if (config.width != width || config.height != height ||
            config.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888 || config.stride < width * 4) {
            Fail(work, "consumer_config");
            break;
        }
        if (work.importMode && !work.importer->Import(nativeBuffer, width, height)) {
            work.importVkResult = work.importer->VkError();
            Fail(work, work.importer->Stage());
            break;
        }
        void* address = nullptr;
        if (OH_NativeBuffer_Map(nativeBuffer, &address) != 0 || !address) {
            Fail(work, "consumer_map");
            break;
        }
        const int32_t xs[] = {0, width / 2, width - 1};
        const int32_t ys[] = {0, height / 2, height - 1};
        valid = true;
        for (int32_t y : ys) {
            for (int32_t x : xs) {
                const auto* pixel = static_cast<const uint8_t*>(address) + y * config.stride + x * 4;
                if (pixel[0] != static_cast<uint8_t>(frame * 31 + 7) ||
                    pixel[1] != 0x5a || pixel[2] != 0xa5 || pixel[3] != 0xff)
                    valid = false;
            }
        }
        if (OH_NativeBuffer_Unmap(nativeBuffer) != 0) {
            Fail(work, "consumer_unmap");
            valid = false;
        } else if (!valid) {
            Fail(work, "pixel_mismatch");
        }
    } while (false);
    if (fence >= 0) close(fence);
    if (OH_NativeImage_ReleaseNativeWindowBuffer(image, windowBuffer, -1) != 0) {
        Fail(work, "release_buffer");
        valid = false;
    }
    OH_NativeWindow_NativeObjectUnreference(windowBuffer);
    return valid;
}

bool FinishChild(OHIPCRemoteProxy* proxy)
{
    OHIPCParcel* request = OH_IPCParcel_Create();
    OHIPCParcel* reply = OH_IPCParcel_Create();
    bool released = false;
    if (request && reply &&
        OH_IPCParcel_WriteInt32(request, kSurfaceProbeVersion) == OH_IPC_SUCCESS &&
        OH_IPCRemoteProxy_SendRequest(proxy, kSurfaceProbeFinish,
                                      request, reply, nullptr) == OH_IPC_SUCCESS) {
        int32_t childResult = -1;
        released = OH_IPCParcel_ReadInt32(reply, &childResult) == OH_IPC_SUCCESS &&
            childResult == 0;
    }
    if (request) OH_IPCParcel_Destroy(request);
    if (reply) OH_IPCParcel_Destroy(reply);
    return released;
}

void ExecuteSurfaceProbe(napi_env, void* data)
{
    auto& work = *static_cast<SurfaceWork*>(data);
    OH_NativeImage* image = OH_ConsumerSurface_Create();
    if (!image) {
        Fail(work, "consumer_create");
        return;
    }
    OHNativeWindow* producer = nullptr;
    bool listenerSet = false;
    do {
        const uint64_t usage = work.gpuMode
            ? NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE
            : NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
        if (OH_ConsumerSurface_SetDefaultSize(image, 64, 64) != 0 ||
            OH_ConsumerSurface_SetDefaultUsage(image, usage) != 0) {
            Fail(work, "consumer_configure");
            break;
        }
        OH_OnFrameAvailableListener listener{};
        listener.context = &work;
        listener.onFrameAvailable = OnFrameAvailable;
        if (OH_NativeImage_SetOnFrameAvailableListener(image, listener) != 0) {
            Fail(work, "consumer_listener");
            break;
        }
        listenerSet = true;
        producer = OH_NativeImage_AcquireNativeWindow(image);
        if (!producer) {
            Fail(work, "producer_window");
            break;
        }
        OH_NativeWindow_NativeWindowHandleOpt(producer, GET_BUFFERQUEUE_SIZE, &work.queueSize);
        if (work.queueSize < 2) {
            Fail(work, "queue_size_below_2");
            break;
        }
        if (work.importMode) {
            work.importer.reset(new (std::nothrow) DirectBufferImportProbe());
            if (!work.importer) {
                Fail(work, "import_probe_alloc");
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_callbackMutex);
            if (g_callbackPending) {
                Fail(work, "create_pending");
                break;
            }
            g_callbackPending = true;
            g_activeWork = &work;
        }
        work.launchCode = OH_Ability_CreateNativeChildProcess(
            work.gpuMode ? "libdirect_gpu_surface_probe.so" : "libdirect_surface_probe.so",
            OnChildStarted);
        if (work.launchCode != NCP_NO_ERROR) {
            std::lock_guard<std::mutex> lock(g_callbackMutex);
            g_activeWork = nullptr;
            g_callbackPending = false;
            Fail(work, "create_ncp");
            break;
        }
        {
            std::unique_lock<std::mutex> lock(g_callbackMutex);
            if (!g_callbackCondition.wait_for(lock, std::chrono::seconds(15),
                                              [&work] { return work.callbackReceived; })) {
                g_activeWork = nullptr;
                Fail(work, "create_callback_timeout");
                break;
            }
            g_activeWork = nullptr;
            g_callbackPending = false;
        }
        if (work.callbackCode != NCP_NO_ERROR || !work.proxy) {
            Fail(work, "create_callback");
            break;
        }
        for (int32_t group = 0; group < 2; ++group) {
            const int32_t firstFrame = group * (work.importMode ? 4 : 3);
            const int32_t width = group == 0 ? 64 : 96;
            const int32_t height = group == 0 ? 64 : 48;
            work.width = width;
            work.height = height;
            if (group == 1 && OH_ConsumerSurface_SetDefaultSize(image, width, height) != 0) {
                Fail(work, "consumer_resize");
                break;
            }
            if (group == 1 && work.importMode) work.importer->NewGeneration();
            if (work.gpuMode) {
                bool groupPassed = true;
                for (int32_t frame = firstFrame;
                     frame < firstFrame + (work.importMode ? 4 : 3); ++frame) {
                    if (!SendFrame(work, producer, frame, width, height) ||
                        !ConsumeFrame(work, image, frame, width, height)) {
                        groupPassed = false;
                        break;
                    }
                    ++work.framesPassed;
                }
                if (!groupPassed) break;
                continue;
            }
            if (!SendFrame(work, producer, firstFrame, width, height)) break;
            const uint32_t firstSeq = work.lastBufferSeq;
            if (!SendFrame(work, producer, firstFrame + 1, width, height)) break;
            const uint32_t secondSeq = work.lastBufferSeq;
            if (firstSeq == secondSeq) {
                Fail(work, "inflight_buffer_reused");
                break;
            }
            if (!ConsumeFrame(work, image, firstFrame, width, height)) break;
            ++work.framesPassed;
            if (!ConsumeFrame(work, image, firstFrame + 1, width, height)) break;
            ++work.framesPassed;
            OH_LOG_INFO(LOG_APP,
                "[DIRECT-D1] in-flight pair=%{public}d child=%{public}d size=%{public}dx%{public}d seq=%{public}u,%{public}u",
                group, work.childPid, width, height, firstSeq, secondSeq);
            if (work.abortMode) {
                work.killCode = OH_Ability_KillChildProcess(work.childPid);
                if (work.killCode == NCP_NO_ERROR) {
                    work.pixelCheck = true;
                    std::snprintf(work.stage, sizeof(work.stage), "aborted_clean");
                } else {
                    Fail(work, "kill_child");
                }
                break;
            }
            if (!SendFrame(work, producer, firstFrame + 2, width, height) ||
                !ConsumeFrame(work, image, firstFrame + 2, width, height))
                break;
            ++work.framesPassed;
        }
        if (work.framesPassed == (work.importMode ? kGpuImportProbeFrameCount :
                                 kSurfaceProbeFrameCount)) {
            work.pixelCheck = true;
            work.importCheck = !work.importMode ||
                (work.importer->ImportCount() >= 2 && work.importer->ReuseCount() >= 2);
            if (!work.importCheck) {
                Fail(work, "import_cache_reuse");
                break;
            }
            std::snprintf(work.stage, sizeof(work.stage), "complete");
        }
    } while (false);
    if (work.proxy) {
        if ((!work.abortMode || work.killCode != NCP_NO_ERROR) &&
            !FinishChild(work.proxy)) {
            work.pixelCheck = false;
            Fail(work, "finish_ipc");
        }
        OH_IPCRemoteProxy_Destroy(work.proxy);
        work.proxy = nullptr;
    }
    if (listenerSet) OH_NativeImage_UnsetOnFrameAvailableListener(image);
    if (work.importer) {
        work.importCount = static_cast<int32_t>(work.importer->ImportCount());
        work.reuseCount = static_cast<int32_t>(work.importer->ReuseCount());
    }
    work.importer.reset();
    OH_NativeImage_Destroy(&image);
}

void CompleteSurfaceProbe(napi_env env, napi_status status, void* data)
{
    auto* work = static_cast<SurfaceWork*>(data);
    if (status != napi_ok) Fail(*work, "async_work");
    napi_value object;
    napi_create_object(env, &object);
    auto setString = [&](const char* key, const char* value) {
        napi_value item;
        napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &item);
        napi_set_named_property(env, object, key, item);
    };
    auto setInt = [&](const char* key, int32_t value) {
        napi_value item;
        napi_create_int32(env, value, &item);
        napi_set_named_property(env, object, key, item);
    };
    setString("gate", work->importMode ? "D2-IMPORT" : work->gpuMode ? "D2-WSI" : "D1");
    setString("status", work->pixelCheck && (!work->importMode || work->importCheck) ? "PASS" : "FAIL");
    setString("stage", work->stage);
    setInt("pid", work->childPid);
    setInt("framesPassed", work->framesPassed);
    setInt("width", work->width);
    setInt("height", work->height);
    setInt("queueSize", work->queueSize);
    setInt("lastBufferSeq", static_cast<int32_t>(work->lastBufferSeq));
    setInt("launchCode", work->launchCode);
    setInt("callbackCode", work->callbackCode);
    setInt("killCode", work->killCode);
    napi_value abortMode;
    napi_get_boolean(env, work->abortMode, &abortMode);
    napi_set_named_property(env, object, "abortMode", abortMode);
    napi_value gpuMode;
    napi_get_boolean(env, work->gpuMode, &gpuMode);
    napi_set_named_property(env, object, "gpuMode", gpuMode);
    napi_value importMode;
    napi_get_boolean(env, work->importMode, &importMode);
    napi_set_named_property(env, object, "importMode", importMode);
    setInt("importVkResult", work->importVkResult);
    napi_value importCheck;
    napi_get_boolean(env, work->importCheck, &importCheck);
    napi_set_named_property(env, object, "importCheck", importCheck);
    setInt("importCount", work->importCount);
    setInt("reuseCount", work->reuseCount);
    setInt("parentFdCount", CountOpenFds());
    setInt("parentRssKiB", ReadRssKiB());
    napi_value check;
    napi_get_boolean(env, work->pixelCheck, &check);
    napi_set_named_property(env, object, "pixelCheck", check);
    napi_resolve_deferred(env, work->deferred, object);
    napi_delete_async_work(env, work->asyncWork);
    delete work;
}

} // namespace

napi_value QueueSurfaceProbe(napi_env env, bool abortMode, bool gpuMode, bool importMode)
{
    auto* work = new (std::nothrow) SurfaceWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate D1 probe work");
        return nullptr;
    }
    work->abortMode = abortMode;
    work->gpuMode = gpuMode;
    work->importMode = importMode;
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        napi_throw_error(env, nullptr, "failed to create D1 probe promise");
        return nullptr;
    }
    napi_value resourceName;
    napi_create_string_utf8(env, "WineHuaDirectD1", NAPI_AUTO_LENGTH, &resourceName);
    if (napi_create_async_work(env, nullptr, resourceName, ExecuteSurfaceProbe,
                               CompleteSurfaceProbe, work, &work->asyncWork) != napi_ok ||
        napi_queue_async_work(env, work->asyncWork) != napi_ok) {
        if (work->asyncWork) napi_delete_async_work(env, work->asyncWork);
        delete work;
        napi_throw_error(env, nullptr, "failed to queue D1 probe");
        return nullptr;
    }
    return promise;
}

napi_value RunSurfaceProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, false, false);
}

napi_value RunSurfaceAbortProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, true, false, false);
}

napi_value RunGpuSurfaceProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, false);
}

napi_value RunGpuImportProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true);
}

} // namespace winehua::direct
