#pragma once

#include <napi/native_api.h>

namespace winehua::direct {

napi_value RunVulkanProbe(napi_env env, napi_callback_info info);
napi_value RunVulkanInlineProbe(napi_env env, napi_callback_info info);
napi_value PreparePhoneDirectForkServer(napi_env env, napi_callback_info info);
napi_value RunVulkanForkServerProbe(napi_env env, napi_callback_info info);
napi_value RunVulkanCreateProbe(napi_env env, napi_callback_info info);
napi_value RunVulkanSystemCreateProbe(napi_env env, napi_callback_info info);

} // namespace winehua::direct
