#pragma once

#include <napi/native_api.h>
#include <cstdint>

namespace winehua::direct {
napi_value SetDirectProbeSurfaceId(napi_env env, napi_callback_info info);
napi_value ClearDirectProbeSurfaceId(napi_env env, napi_callback_info info);
napi_value SetDirectProbeSurfaceSize(napi_env env, napi_callback_info info);
napi_value TakeDirectProbeResizeRequest(napi_env env, napi_callback_info info);
napi_value RunDirectOutputProbe(napi_env env, napi_callback_info info);
bool WaitDirectProbeSurfaceId(uint64_t* surfaceId, uint32_t timeoutMs);
bool RequestDirectProbeResize(uint64_t surfaceId, uint32_t oldWidth, uint32_t oldHeight,
                              uint32_t* newWidth, uint32_t* newHeight, uint32_t timeoutMs);
} // namespace winehua::direct
