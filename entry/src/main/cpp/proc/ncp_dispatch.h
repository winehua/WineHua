// Explicit app-owned NCP dispatch. Never interpose the OH_Ability_* SDK names:
// libchild_process.so can precede libentry.so in the dynamic linker's scope.
#pragma once
#include <AbilityKit/native_child_process.h>

extern "C" {
__attribute__((visibility("hidden")))
Ability_NativeChildProcess_ErrCode WineHua_StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args,
    NativeChildProcess_Options options, int32_t* pid);

__attribute__((visibility("hidden")))
int WineHua_CreateNativeChildProcess(
    const char* libName, OH_Ability_OnNativeChildProcessStarted onProcessStarted);
}
