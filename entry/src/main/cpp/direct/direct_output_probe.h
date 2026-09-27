#pragma once

#include <napi/native_api.h>
#include <cstdint>

namespace winehua::direct {
napi_value SetDirectProbeSurfaceId(napi_env env, napi_callback_info info);
napi_value ClearDirectProbeSurfaceId(napi_env env, napi_callback_info info);
napi_value RunDirectOutputProbe(napi_env env, napi_callback_info info);
bool WaitDirectProbeSurfaceId(uint64_t* surfaceId, uint32_t timeoutMs);
} // namespace winehua::direct
