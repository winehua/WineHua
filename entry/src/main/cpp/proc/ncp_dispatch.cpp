// ncp_dispatch.cpp — NCP 路由层：手机 vs 非手机
//
// g_isPhone: 由 ArkTS EntryAbility 通过 NAPI SetPhoneMode 设置。
//   true  = 手机（系统 NCP 不可用）→ 走 phone_adapter/ 的 fork 实现
//   false = 2in1/Pad（系统 NCP 可用）→ 走系统 libchild_process.so
//
// 手机 fork 实现（Phone_Start/Create）在 phone_adapter/phone_process.cpp。
// virgl IPC relay/dispatch 在 phone_adapter/phone_virgl_relay.cpp, phone_virgl_dispatch.cpp。
#include <AbilityKit/native_child_process.h>
#include <dlfcn.h>

#include "phone_adapter/phone_process.h"

static bool g_isPhone = false;

extern "C" void PhoneAdapter_SetPhoneMode(bool phone) {
    g_isPhone = phone;
}

extern "C" bool PhoneAdapter_IsPhoneMode() {
    return g_isPhone;
}

// 获取系统 libchild_process.so 的原始 NCP 函数指针。
// 不能用 RTLD_NEXT（libchild_process.so 在 entry.so 之前加载，NEXT 搜不到），
// 改用 dlopen(NOLOAD) 获取已加载的 libchild_process.so 句柄再 dlsym。
template<typename Fn>
static Fn GetRealNcp(const char* name) {
    void* h = dlopen("libchild_process.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return nullptr;
    return reinterpret_cast<Fn>(dlsym(h, name));
}

extern "C" {

// ====== 路由 wrapper（符号覆盖点，覆盖 libchild_process.so 的实现）======
// g_isPhone=true  → phone_adapter/ fork 实现
// g_isPhone=false → 系统 libchild_process.so 原始实现

Ability_NativeChildProcess_ErrCode OH_Ability_StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args,
    NativeChildProcess_Options options, int32_t* pid)
{
    if (!g_isPhone) {
        auto realFn = GetRealNcp<decltype(&OH_Ability_StartNativeChildProcess)>(
            "OH_Ability_StartNativeChildProcess");
        if (realFn) return realFn(entry, args, options, pid);
        return NCP_ERR_INTERNAL;
    }
    return Phone_StartNativeChildProcess(entry, args, options, pid);
}

int OH_Ability_CreateNativeChildProcess(
    const char* libName, OH_Ability_OnNativeChildProcessStarted onProcessStarted)
{
    if (!g_isPhone) {
        auto realFn = GetRealNcp<decltype(&OH_Ability_CreateNativeChildProcess)>(
            "OH_Ability_CreateNativeChildProcess");
        if (realFn) return realFn(libName, onProcessStarted);
        return NCP_ERR_INTERNAL;
    }
    return Phone_CreateNativeChildProcess(libName, onProcessStarted);
}

} // extern "C"
