// phone_adapter.h — 手机适配层总头文件
// 手机适配层按功能拆分为子模块：进程创建（phone_process）、virgl relay（phone_virgl_relay）、
// virgl dispatch（phone_virgl_dispatch）、socket 工具（phone_socket，header-only inline）。
// 显式进程路由入口在 proc/ncp_dispatch.h / ncp_dispatch.cpp。
#pragma once
#include "phone_socket.h"
#include "phone_process.h"
#include "phone_virgl_relay.h"
#define PHONE_ADAPTER_DUMMY_PROXY ((void*)0x4E435031)

// 由 ncp_dispatch.cpp 实现，napi_init.cpp 通过 NAPI 桥接调用
#ifdef __cplusplus
extern "C" {
#endif
void PhoneAdapter_SetPhoneMode(bool phone);
bool PhoneAdapter_IsPhoneMode();
#ifdef __cplusplus
}
#endif
