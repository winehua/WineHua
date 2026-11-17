#pragma once
#include <cstdint>
struct OH_NativeVSync;
struct OH_NativeVSync_ExpectedRateRange { int32_t min, max, expected; };
using OH_NativeVSync_FrameCallback = void (*)(long long, void*);
OH_NativeVSync* OH_NativeVSync_Create(const char*, unsigned);
void OH_NativeVSync_Destroy(OH_NativeVSync*);
int OH_NativeVSync_SetExpectedFrameRateRange(OH_NativeVSync*, OH_NativeVSync_ExpectedRateRange*);
int OH_NativeVSync_RequestFrame(OH_NativeVSync*, OH_NativeVSync_FrameCallback, void*);
int OH_NativeVSync_GetPeriod(OH_NativeVSync*, long long*);
