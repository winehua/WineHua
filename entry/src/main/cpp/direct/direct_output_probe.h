#pragma once

#include <napi/native_api.h>

namespace winehua::direct {
napi_value SetDirectProbeSurfaceId(napi_env env, napi_callback_info info);
napi_value ClearDirectProbeSurfaceId(napi_env env, napi_callback_info info);
napi_value RunDirectOutputProbe(napi_env env, napi_callback_info info);
} // namespace winehua::direct
