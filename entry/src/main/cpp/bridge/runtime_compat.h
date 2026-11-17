#pragma once
#include <napi/native_api.h>

// Preserve WineHua master's entry points while sharing the new runtime paths.
void RegisterRuntimeCompat(napi_env env, napi_value exports);
