#include "proc/wine_child_ipc_launcher.h"
#include "proc/wine_process.h"

#include <napi/native_api.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <native_buffer/native_buffer.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <new>
#include <string>
#include <thread>

namespace winehua::direct {
namespace {

struct SurfaceWork {
    napi_async_work asyncWork = nullptr;
    napi_deferred deferred = nullptr;
    int32_t pid = -1;
    int32_t launchCode = -1;
    bool firstAttach = false;
    bool directRoute = false;
    bool firstQuery = false;
    bool staleRejected = false;
    bool staleAttachAccepted = false;
    bool firstQueryAfterStale = false;
    bool resized = false;
    bool oldGenerationRejected = false;
    bool detach = false;
    bool detachedQueryRejected = false;
    bool deathReceived = false;
    const char* stage = "pending";
};

void Execute(napi_env, void* data)
{
    auto& work = *static_cast<SurfaceWork*>(data);
    NativeChildProcess_Args args{};
    const std::string params = std::string(wineipc::kSurfaceProbeParams) +
        "|__env=WINEHUA_VULKAN_BACKEND=direct";
    args.entryParams = const_cast<char*>(params.c_str());
    work.launchCode = StartWineChildViaIpc(args, &work.pid);
    if (work.launchCode != NCP_NO_ERROR || work.pid <= 0) {
        work.stage = "create_wine_ncp";
        return;
    }
    AddProcess(work.pid, wineipc::kSurfaceProbeParams, -1);
    MarkWineIpcChildRegistered(work.pid);
    work.directRoute = WineIpcChildUsesDirectVulkan(work.pid);
    if (!work.directRoute) { work.stage = "direct_route"; (void)FinishWineDirectSurfaceProbe(work.pid); return; }

    OH_NativeImage* image = OH_ConsumerSurface_Create();
    OH_NativeImage* resizedImage = nullptr;
    OHNativeWindow* producer = nullptr;
    do {
        if (!image || OH_ConsumerSurface_SetDefaultSize(image, 64, 64) != 0 ||
            OH_ConsumerSurface_SetDefaultUsage(image,
                NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE) != 0 ||
            !(producer = OH_NativeImage_AcquireNativeWindow(image))) {
            work.stage = "create_consumer";
            break;
        }
        wineipc::DirectSurfaceToken first{work.pid, 41, 71, 1, 64, 64};
        work.firstAttach = AttachWineDirectSurface(first, producer);
        if (!work.firstAttach) { work.stage = "attach_first"; break; }
        work.firstQuery = QueryWineDirectSurface(first);
        if (!work.firstQuery) { work.stage = "query_first"; break; }

        auto stale = first;
        stale.wlSurfaceId = 72;
        work.staleAttachAccepted = AttachWineDirectSurface(stale, producer);
        work.firstQueryAfterStale = QueryWineDirectSurface(first);
        work.staleRejected = !work.staleAttachAccepted && work.firstQueryAfterStale;
        if (!work.staleRejected) { work.stage = "stale_rejection"; break; }

        auto second = first;
        second.generation = 2;
        second.width = 80;
        second.height = 80;
        resizedImage = OH_ConsumerSurface_Create();
        if (!resizedImage || OH_ConsumerSurface_SetDefaultSize(resizedImage, 80, 80) != 0 ||
            OH_ConsumerSurface_SetDefaultUsage(resizedImage,
                NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE) != 0) {
            work.stage = "create_resized_consumer";
            break;
        }
        OHNativeWindow* resizedProducer = OH_NativeImage_AcquireNativeWindow(resizedImage);
        if (!resizedProducer) { work.stage = "resized_producer"; break; }
        work.resized = AttachWineDirectSurface(second, resizedProducer) &&
                       QueryWineDirectSurface(second);
        if (!work.resized) { work.stage = "attach_resize"; break; }
        work.oldGenerationRejected = !QueryWineDirectSurface(first) &&
                                     !DetachWineDirectSurface(first) &&
                                     QueryWineDirectSurface(second);
        if (!work.oldGenerationRejected) { work.stage = "old_generation"; break; }
        work.detach = DetachWineDirectSurface(second);
        if (!work.detach) { work.stage = "detach"; break; }
        work.detachedQueryRejected = !QueryWineDirectSurface(second);
        work.stage = work.detachedQueryRejected ? "complete" : "query_after_detach";
    } while (false);
    if (resizedImage) OH_NativeImage_Destroy(&resizedImage);
    if (image) OH_NativeImage_Destroy(&image);
    (void)FinishWineDirectSurfaceProbe(work.pid);
    for (int i = 0; i < 50; ++i) {
        if (IsPidExited(work.pid)) { work.deathReceived = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!work.deathReceived && std::strcmp(work.stage, "complete") == 0)
        work.stage = "death_timeout";
}

void SetInt(napi_env env, napi_value object, const char* key, int32_t value)
{
    napi_value field;
    napi_create_int32(env, value, &field);
    napi_set_named_property(env, object, key, field);
}

void SetString(napi_env env, napi_value object, const char* key, const char* value)
{
    napi_value field;
    napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &field);
    napi_set_named_property(env, object, key, field);
}

void Complete(napi_env env, napi_status status, void* data)
{
    auto* work = static_cast<SurfaceWork*>(data);
    if (status != napi_ok) work->stage = "async_work";
    napi_value result;
    napi_create_object(env, &result);
    SetString(env, result, "gate", "D3-WINE-SURFACE-IPC");
    SetString(env, result, "status", std::strcmp(work->stage, "complete") == 0 ? "PASS" : "FAIL");
    SetString(env, result, "stage", work->stage);
    SetInt(env, result, "pid", work->pid);
    SetInt(env, result, "launchCode", work->launchCode);
    SetInt(env, result, "directRoute", work->directRoute);
    SetInt(env, result, "firstAttach", work->firstAttach);
    SetInt(env, result, "firstQuery", work->firstQuery);
    SetInt(env, result, "staleRejected", work->staleRejected);
    SetInt(env, result, "staleAttachAccepted", work->staleAttachAccepted);
    SetInt(env, result, "firstQueryAfterStale", work->firstQueryAfterStale);
    SetInt(env, result, "resized", work->resized);
    SetInt(env, result, "oldGenerationRejected", work->oldGenerationRejected);
    SetInt(env, result, "detach", work->detach);
    SetInt(env, result, "detachedQueryRejected", work->detachedQueryRejected);
    SetInt(env, result, "deathReceived", work->deathReceived);
    napi_resolve_deferred(env, work->deferred, result);
    napi_delete_async_work(env, work->asyncWork);
    delete work;
}

} // namespace

napi_value RunWineSurfaceIpcProbe(napi_env env, napi_callback_info)
{
    auto* work = new (std::nothrow) SurfaceWork();
    if (!work) return nullptr;
    napi_value promise;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) {
        delete work;
        return nullptr;
    }
    napi_value name;
    napi_create_string_utf8(env, "WineHuaDirectWineSurfaceIpc", NAPI_AUTO_LENGTH, &name);
    if (napi_create_async_work(env, nullptr, name, Execute, Complete,
                               work, &work->asyncWork) != napi_ok ||
        napi_queue_async_work(env, work->asyncWork) != napi_ok) {
        if (work->asyncWork) napi_delete_async_work(env, work->asyncWork);
        delete work;
        napi_throw_error(env, nullptr, "D3 Wine surface IPC probe queue failed");
        return nullptr;
    }
    return promise;
}

} // namespace winehua::direct
