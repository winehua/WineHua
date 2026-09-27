#pragma once

#include <napi/native_api.h>

namespace winehua::direct {
napi_value RunSurfaceProbe(napi_env env, napi_callback_info info);
napi_value RunSurfaceAbortProbe(napi_env env, napi_callback_info info);
napi_value RunGpuSurfaceProbe(napi_env env, napi_callback_info info);
napi_value RunGpuImportProbe(napi_env env, napi_callback_info info);
napi_value RunGpuSampleProbe(napi_env env, napi_callback_info info);
napi_value RunGpuFenceProbe(napi_env env, napi_callback_info info);
napi_value RunGpuCompositeProbe(napi_env env, napi_callback_info info);
napi_value RunGpuCompositeResizeProbe(napi_env env, napi_callback_info info);
napi_value RunGpuCompositeThroughputProbe(napi_env env, napi_callback_info info);
napi_value RunGpuProducerPipelineProbe(napi_env env, napi_callback_info info);
napi_value RunGpuDualSlotProbe(napi_env env, napi_callback_info info);
} // namespace winehua::direct
