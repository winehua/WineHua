#pragma once
#include <napi/native_api.h>
#include <string>
void RegisterSystemOpen(napi_env env, napi_value exports);
bool DispatchSystemOpen(const std::string& target);
void ResetSystemOpenRequests();
