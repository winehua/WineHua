// phone_process.h — 手机适配层：fork 进程创建
#pragma once
#include <AbilityKit/native_child_process.h>

// fork 版进程创建（纯手机代码，无路由逻辑）
Ability_NativeChildProcess_ErrCode Phone_StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args,
    NativeChildProcess_Options options, int32_t* pid);

int Phone_CreateNativeChildProcess(
    const char* libName, OH_Ability_OnNativeChildProcessStarted onProcessStarted);

// Explicit Direct diagnostics only. Prepare synchronously in Ability.onCreate,
// before ArkUI initializes GPU state; later requests fork from this clean server.
int Phone_PrepareDirectForkServer();
int32_t Phone_GetDirectForkServerPid();
// Unlike the legacy phone Start adapter, the caller retains all supplied fds.
Ability_NativeChildProcess_ErrCode Phone_StartViaDirectForkServer(
    const char* entry, NativeChildProcess_Args args, int32_t* pid);
bool Phone_QueryDirectForkChildExit(int32_t pid, int* waitStatus);
bool Phone_IsDirectForkServerChild(int32_t pid);
void Phone_MarkDirectForkChildRegistered(int32_t pid);

// Proxy 查询接口（graphics_broker.cpp 使用）
bool PhoneAdapter_IsDummyProxy(const void* p);
int  PhoneAdapter_GetConfigSocket(void);
void PhoneAdapter_CloseConfigSocket(void);
