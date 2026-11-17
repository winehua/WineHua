#pragma once
using napi_threadsafe_function = void*;
enum napi_threadsafe_function_call_mode { napi_tsfn_blocking };
int ProcessTestStateCallback(void* data);
inline int napi_call_threadsafe_function(napi_threadsafe_function, void* data,
                                        napi_threadsafe_function_call_mode) {
    return ProcessTestStateCallback(data);
}
