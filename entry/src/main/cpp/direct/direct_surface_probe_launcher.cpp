#include "direct_surface_probe_launcher.h"
#include "direct_buffer_import_probe.h"
#include "direct_output_probe.h"
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
#include <algorithm>
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
#include <vector>

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
    bool sampleMode = false;
    bool sampleCheck = false;
    bool fenceMode = false;
    bool fenceCheck = false;
    bool compositeMode = false;
    bool resizeMode = false;
    bool throughputMode = false;
    bool pipelineMode = false;
    bool dualSlotMode = false;
    int32_t bufferedFramesPeak = 0;
    int32_t consumerSlotsPeak = 0;
    int32_t frameTarget = kGpuImportProbeFrameCount;
    double elapsedMs = 0.0;
    double fps = 0.0;
    double frameP50Ms = 0.0;
    double frameP95Ms = 0.0;
    double frameP99Ms = 0.0;
    double producerP95Ms = 0.0;
    double consumerP95Ms = 0.0;
    bool outputCheck = false;
    uint64_t outputSurfaceId = 0;
    int32_t outputPresentCount = 0;
    int32_t outputWidth = 0;
    int32_t outputHeight = 0;
    int32_t outputInitialWidth = 0;
    int32_t outputInitialHeight = 0;
    int32_t outputRecreateCount = 0;
    int32_t importVkResult = 0;
    int32_t importCount = 0;
    int32_t reuseCount = 0;
    int32_t sampleCount = 0;
    int32_t acquireImportCount = 0;
    int32_t releaseExportCount = 0;
    int32_t releaseFdCount = 0;
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

double PercentileMs(std::vector<double> samples, uint32_t percentile)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t rank = (static_cast<size_t>(percentile) * samples.size() + 99) / 100;
    return samples[std::min(samples.size() - 1, std::max<size_t>(rank, 1) - 1)];
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
                  int32_t frame, int32_t width, int32_t height,
                  bool deferFinish = false)
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
    bool released = false;
    do {
        if (!work.fenceMode) {
            const bool fenceReady = WaitFence(fence);
            fence = -1;
            if (!fenceReady) {
                Fail(work, "acquire_fence");
                break;
            }
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
        if (work.fenceMode) {
            int releaseFence = -1;
            if (!work.importer->SubmitSampleWithFences(nativeBuffer, width, height, frame,
                                                       &fence, &releaseFence)) {
                work.importVkResult = work.importer->VkError();
                Fail(work, work.importer->Stage());
                break;
            }
            if (releaseFence >= 0) ++work.releaseFdCount;
            if (OH_NativeImage_ReleaseNativeWindowBuffer(image, windowBuffer,
                                                         releaseFence) != 0) {
                if (releaseFence >= 0) close(releaseFence);
                work.importer->FinishSample(frame);
                Fail(work, "release_buffer_fence");
                break;
            }
            released = true;
            valid = deferFinish || work.importer->FinishSample(frame);
            if (!valid) {
                work.importVkResult = work.importer->VkError();
                Fail(work, work.importer->Stage());
            }
            break;
        }
        if (work.sampleMode && !work.importer->Sample(nativeBuffer, width, height, frame)) {
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
    if (!released && OH_NativeImage_ReleaseNativeWindowBuffer(image, windowBuffer, -1) != 0) {
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
            if (work.compositeMode &&
                !WaitDirectProbeSurfaceId(&work.outputSurfaceId, 10000)) {
                Fail(work, "composite_surface_timeout");
                break;
            }
            work.importer.reset(new (std::nothrow) DirectBufferImportProbe(
                work.fenceMode, work.outputSurfaceId, work.dualSlotMode ? 2u : 1u));
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
        if (work.throughputMode) {
            std::vector<double> frameMs;
            std::vector<double> producerMs;
            std::vector<double> consumerMs;
            frameMs.reserve(work.frameTarget);
            producerMs.reserve(work.frameTarget);
            consumerMs.reserve(work.frameTarget);
            work.width = 64;
            work.height = 64;
            const auto begin = std::chrono::steady_clock::now();
            if (work.pipelineMode) {
                std::vector<std::chrono::steady_clock::time_point> frameStarts(work.frameTarget);
                bool ready = true;
                uint32_t firstSeq = 0;
                for (int32_t frame = 0; frame < 2; ++frame) {
                    frameStarts[frame] = std::chrono::steady_clock::now();
                    if (!SendFrame(work, producer, frame, 64, 64)) {
                        ready = false;
                        break;
                    }
                    producerMs.push_back(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - frameStarts[frame]).count());
                    if (frame == 0) firstSeq = work.lastBufferSeq;
                    if (frame == 1) {
                        if (firstSeq == work.lastBufferSeq) {
                            Fail(work, "pipeline_buffer_reused");
                            ready = false;
                        } else {
                            work.bufferedFramesPeak = 2;
                        }
                    }
                }
                if (ready) {
                    int32_t submitted = 0;
                    for (int32_t frame = 0; frame < work.frameTarget; ++frame) {
                        if (work.dualSlotMode && frame >= 2) {
                            const int32_t completedFrame = frame - 2;
                            if (!work.importer->FinishSample(completedFrame)) {
                                work.importVkResult = work.importer->VkError();
                                Fail(work, work.importer->Stage());
                                break;
                            }
                            frameMs.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                frameStarts[completedFrame]).count());
                            ++work.framesPassed;
                        }
                        const auto consumerBegin = std::chrono::steady_clock::now();
                        if (!ConsumeFrame(work, image, frame, 64, 64,
                                          work.dualSlotMode)) break;
                        ++submitted;
                        const auto consumed = std::chrono::steady_clock::now();
                        consumerMs.push_back(std::chrono::duration<double, std::milli>(
                            consumed - consumerBegin).count());
                        if (work.dualSlotMode) {
                            if (submitted == 2) work.consumerSlotsPeak = 2;
                        } else {
                            frameMs.push_back(std::chrono::duration<double, std::milli>(
                                consumed - frameStarts[frame]).count());
                            ++work.framesPassed;
                        }
                        const int32_t next = frame + 2;
                        if (next < work.frameTarget) {
                            frameStarts[next] = std::chrono::steady_clock::now();
                            if (!SendFrame(work, producer, next, 64, 64)) break;
                            producerMs.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - frameStarts[next]).count());
                        }
                    }
                    if (work.dualSlotMode && submitted == work.frameTarget) {
                        for (int32_t frame = work.frameTarget - 2;
                             frame < work.frameTarget; ++frame) {
                            if (!work.importer->FinishSample(frame)) {
                                work.importVkResult = work.importer->VkError();
                                Fail(work, work.importer->Stage());
                                break;
                            }
                            frameMs.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - frameStarts[frame]).count());
                            ++work.framesPassed;
                        }
                    }
                }
            } else {
                for (int32_t frame = 0; frame < work.frameTarget; ++frame) {
                    const auto frameBegin = std::chrono::steady_clock::now();
                    if (!SendFrame(work, producer, frame, 64, 64)) break;
                    const auto produced = std::chrono::steady_clock::now();
                    if (!ConsumeFrame(work, image, frame, 64, 64)) break;
                    const auto consumed = std::chrono::steady_clock::now();
                    frameMs.push_back(std::chrono::duration<double, std::milli>(
                        consumed - frameBegin).count());
                    producerMs.push_back(std::chrono::duration<double, std::milli>(
                        produced - frameBegin).count());
                    consumerMs.push_back(std::chrono::duration<double, std::milli>(
                        consumed - produced).count());
                    ++work.framesPassed;
                }
            }
            const auto end = std::chrono::steady_clock::now();
            work.elapsedMs = std::chrono::duration<double, std::milli>(end - begin).count();
            if (work.elapsedMs > 0.0)
                work.fps = static_cast<double>(work.framesPassed) * 1000.0 / work.elapsedMs;
            work.frameP50Ms = PercentileMs(frameMs, 50);
            work.frameP95Ms = PercentileMs(frameMs, 95);
            work.frameP99Ms = PercentileMs(frameMs, 99);
            work.producerP95Ms = PercentileMs(producerMs, 95);
            work.consumerP95Ms = PercentileMs(consumerMs, 95);
        } else {
        for (int32_t group = 0; group < 2; ++group) {
            const int32_t firstFrame = group * (work.importMode ? 4 : 3);
            const int32_t width = group == 0 ? 64 : 96;
            const int32_t height = group == 0 ? 64 : 48;
            work.width = width;
            work.height = height;
            if (group == 1 && work.resizeMode) {
                work.outputInitialWidth = static_cast<int32_t>(work.importer->OutputWidth());
                work.outputInitialHeight = static_cast<int32_t>(work.importer->OutputHeight());
                uint32_t resizedWidth = 0;
                uint32_t resizedHeight = 0;
                if (!RequestDirectProbeResize(work.outputSurfaceId,
                                              static_cast<uint32_t>(work.outputInitialWidth),
                                              static_cast<uint32_t>(work.outputInitialHeight),
                                              &resizedWidth, &resizedHeight, 10000)) {
                    Fail(work, "composite_resize_callback_timeout");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!work.importer->RecreateOutput()) {
                    work.importVkResult = work.importer->VkError();
                    Fail(work, work.importer->Stage());
                    break;
                }
                if (work.importer->OutputWidth() != resizedWidth ||
                    work.importer->OutputHeight() != resizedHeight) {
                    Fail(work, "composite_resize_extent");
                    break;
                }
            }
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
        }
        if (work.framesPassed == work.frameTarget) {
            work.pixelCheck = true;
            work.importCheck = !work.importMode ||
                (work.importer->ImportCount() >= 2 && work.importer->ReuseCount() >= 2);
            if (!work.importCheck) {
                Fail(work, "import_cache_reuse");
                break;
            }
            work.sampleCheck = !work.sampleMode ||
                work.importer->SampleCount() == static_cast<uint32_t>(work.frameTarget);
            if (!work.sampleCheck) {
                Fail(work, "sample_count");
                break;
            }
            work.fenceCheck = !work.fenceMode ||
                (work.importer->AcquireImportCount() > 0 &&
                 work.importer->ReleaseExportCount() == static_cast<uint32_t>(work.frameTarget) &&
                 work.releaseFdCount > 0);
            if (!work.fenceCheck) {
                Fail(work, "fence_not_exercised");
                break;
            }
            work.outputCheck = !work.compositeMode ||
                (work.importer->OutputPresentCount() == static_cast<uint32_t>(work.frameTarget) &&
                 (!work.resizeMode ||
                  (work.importer->OutputRecreateCount() == 1 &&
                   work.importer->OutputWidth() != static_cast<uint32_t>(work.outputInitialWidth) &&
                   work.importer->OutputHeight() != static_cast<uint32_t>(work.outputInitialHeight))));
            if (!work.outputCheck) {
                Fail(work, "composite_present_count");
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
        work.sampleCount = static_cast<int32_t>(work.importer->SampleCount());
        work.acquireImportCount = static_cast<int32_t>(work.importer->AcquireImportCount());
        work.releaseExportCount = static_cast<int32_t>(work.importer->ReleaseExportCount());
        work.outputPresentCount = static_cast<int32_t>(work.importer->OutputPresentCount());
        work.outputWidth = static_cast<int32_t>(work.importer->OutputWidth());
        work.outputHeight = static_cast<int32_t>(work.importer->OutputHeight());
        work.outputRecreateCount = static_cast<int32_t>(work.importer->OutputRecreateCount());
    }
    if (work.compositeMode && work.outputCheck)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    setString("gate", work->dualSlotMode ? "D2-DUAL-SLOT" :
              work->pipelineMode ? "D2-PRODUCER-PIPELINE" :
              work->throughputMode ? "D2-THROUGHPUT" :
              work->resizeMode ? "D2-COMPOSITE-RESIZE" :
              work->compositeMode ? "D2-COMPOSITE" :
              work->fenceMode ? "D2-FENCE" : work->sampleMode ? "D2-SAMPLE" :
              work->importMode ? "D2-IMPORT" :
              work->gpuMode ? "D2-WSI" : "D1");
    setString("status", work->pixelCheck && (!work->importMode || work->importCheck) &&
              (!work->sampleMode || work->sampleCheck) &&
              (!work->fenceMode || work->fenceCheck) &&
              (!work->pipelineMode || work->bufferedFramesPeak == 2) &&
              (!work->dualSlotMode || work->consumerSlotsPeak == 2) &&
              (!work->compositeMode || work->outputCheck) ? "PASS" : "FAIL");
    setString("stage", work->stage);
    setInt("pid", work->childPid);
    setInt("framesPassed", work->framesPassed);
    setInt("frameTarget", work->frameTarget);
    setInt("bufferedFramesPeak", work->bufferedFramesPeak);
    setInt("consumerSlotsPeak", work->consumerSlotsPeak);
    auto setDouble = [&](const char* key, double value) {
        napi_value item;
        napi_create_double(env, value, &item);
        napi_set_named_property(env, object, key, item);
    };
    setDouble("elapsedMs", work->elapsedMs);
    setDouble("fps", work->fps);
    setDouble("frameP50Ms", work->frameP50Ms);
    setDouble("frameP95Ms", work->frameP95Ms);
    setDouble("frameP99Ms", work->frameP99Ms);
    setDouble("producerP95Ms", work->producerP95Ms);
    setDouble("consumerP95Ms", work->consumerP95Ms);
    napi_value throughputAtLeast60;
    napi_get_boolean(env, work->throughputMode && work->fps >= 60.0, &throughputAtLeast60);
    napi_set_named_property(env, object, "throughputAtLeast60", throughputAtLeast60);
    napi_value pipelineMode;
    napi_get_boolean(env, work->pipelineMode, &pipelineMode);
    napi_set_named_property(env, object, "pipelineMode", pipelineMode);
    napi_value dualSlotMode;
    napi_get_boolean(env, work->dualSlotMode, &dualSlotMode);
    napi_set_named_property(env, object, "dualSlotMode", dualSlotMode);
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
    setInt("sampleCount", work->sampleCount);
    setInt("acquireImportCount", work->acquireImportCount);
    setInt("releaseExportCount", work->releaseExportCount);
    setInt("releaseFdCount", work->releaseFdCount);
    setInt("outputPresentCount", work->outputPresentCount);
    setInt("outputWidth", work->outputWidth);
    setInt("outputHeight", work->outputHeight);
    setInt("outputInitialWidth", work->outputInitialWidth);
    setInt("outputInitialHeight", work->outputInitialHeight);
    setInt("outputRecreateCount", work->outputRecreateCount);
    char surfaceIdText[32]{};
    std::snprintf(surfaceIdText, sizeof(surfaceIdText), "%llu",
                  static_cast<unsigned long long>(work->outputSurfaceId));
    setString("outputSurfaceId", surfaceIdText);
    napi_value fenceCheck;
    napi_get_boolean(env, work->fenceCheck, &fenceCheck);
    napi_set_named_property(env, object, "fenceCheck", fenceCheck);
    napi_value sampleCheck;
    napi_get_boolean(env, work->sampleCheck, &sampleCheck);
    napi_set_named_property(env, object, "sampleCheck", sampleCheck);
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

napi_value QueueSurfaceProbe(napi_env env, bool abortMode, bool gpuMode,
                             bool importMode, bool sampleMode, bool fenceMode,
                             bool compositeMode = false, bool resizeMode = false,
                             bool throughputMode = false, bool pipelineMode = false,
                             bool dualSlotMode = false)
{
    auto* work = new (std::nothrow) SurfaceWork();
    if (!work) {
        napi_throw_error(env, nullptr, "failed to allocate D1 probe work");
        return nullptr;
    }
    work->abortMode = abortMode;
    work->gpuMode = gpuMode;
    work->importMode = importMode;
    work->sampleMode = sampleMode;
    work->fenceMode = fenceMode;
    work->compositeMode = compositeMode;
    work->resizeMode = resizeMode;
    work->throughputMode = throughputMode;
    work->pipelineMode = pipelineMode;
    work->dualSlotMode = dualSlotMode;
    work->frameTarget = throughputMode ? kGpuThroughputProbeFrameCount :
        importMode ? kGpuImportProbeFrameCount : kSurfaceProbeFrameCount;
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
    return QueueSurfaceProbe(env, false, false, false, false, false);
}

napi_value RunSurfaceAbortProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, true, false, false, false, false);
}

napi_value RunGpuSurfaceProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, false, false, false);
}

napi_value RunGpuImportProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, false, false);
}

napi_value RunGpuSampleProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, false);
}

napi_value RunGpuFenceProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, true);
}

napi_value RunGpuCompositeProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, true, true);
}

napi_value RunGpuCompositeResizeProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, true, true, true);
}

napi_value RunGpuCompositeThroughputProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, true, true, false, true);
}

napi_value RunGpuProducerPipelineProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, true, true, false, true, true);
}

napi_value RunGpuDualSlotProbe(napi_env env, napi_callback_info)
{
    return QueueSurfaceProbe(env, false, true, true, true, true, true, false, true, true, true);
}

} // namespace winehua::direct
