#pragma once
#include <napi/native_api.h>

namespace winehua::direct {
napi_value RunPhoneSharedBufferProbe(napi_env env, napi_callback_info info);
napi_value ProbeProductDirectSupport(napi_env env, napi_callback_info info);
}
