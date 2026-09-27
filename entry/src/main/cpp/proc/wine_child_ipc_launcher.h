#pragma once

#include <AbilityKit/native_child_process.h>
#include <cstdint>

// Opt-in Create NCP launcher. The caller still owns every fd in args after return.
// On success, call MarkWineIpcChildRegistered after AddProcess to enable death delivery.
int32_t StartWineChildViaIpc(const NativeChildProcess_Args& args, int32_t* childPid);
void MarkWineIpcChildRegistered(int32_t childPid);
