#pragma once

#include <napi/native_api.h>

// Controller Hub NAPI (Touch source + enable/WHGP bootstrap).
napi_value ControllerSetEnabled(napi_env env, napi_callback_info info);
napi_value ControllerSetButton(napi_env env, napi_callback_info info);
napi_value ControllerSetAxis(napi_env env, napi_callback_info info);
napi_value ControllerSetHat(napi_env env, napi_callback_info info);
napi_value ControllerResetSource(napi_env env, napi_callback_info info);
napi_value ControllerGetState(napi_env env, napi_callback_info info);
napi_value ControllerGetStateText(napi_env env, napi_callback_info info);
napi_value ControllerStartBridge(napi_env env, napi_callback_info info);
napi_value ControllerStopBridge(napi_env env, napi_callback_info info);
napi_value ControllerGetSocketPath(napi_env env, napi_callback_info info);
napi_value ControllerSetOutputMode(napi_env env, napi_callback_info info);
napi_value ControllerGetOutputMode(napi_env env, napi_callback_info info);
