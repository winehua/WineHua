#pragma once

#include <napi/native_api.h>

namespace winehua::direct {
napi_value RunWineIpcProbe(napi_env env, napi_callback_info info);
}
