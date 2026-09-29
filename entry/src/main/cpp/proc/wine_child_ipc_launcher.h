#pragma once

#include <AbilityKit/native_child_process.h>
#include <cstdint>
#include <native_window/external_window.h>
#include "wine_child_ipc.h"

// Opt-in Create NCP launcher. The caller still owns every fd in args after return.
// On success, call MarkWineIpcChildRegistered after AddProcess to enable death delivery.
int32_t StartWineChildViaIpc(const NativeChildProcess_Args& args, int32_t* childPid);
void MarkWineIpcChildRegistered(int32_t childPid);
bool WineIpcChildUsesDirectVulkan(int32_t childPid);
// The caller owns producerWindow. These calls are valid only for a live Create
// NCP and never route through the legacy Start path.
bool AttachWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token,
                             OHNativeWindow* producerWindow);
bool DetachWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token);
bool ResizeWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token);
bool QueryWineDirectSurface(const winehua::wineipc::DirectSurfaceToken& token);
bool FinishWineDirectSurfaceProbe(int32_t childPid);
