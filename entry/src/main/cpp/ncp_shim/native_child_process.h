#ifndef WINEHUA_NCP_BACKEND_H
#define WINEHUA_NCP_BACKEND_H

#include <AbilityKit/native_child_process.h>

namespace winehua::ncp {

// The fork backend is opt-in. System NCP remains the default for tablet,
// 2-in-1, and PC devices where appspawn and Binder are available.
void SetForkBackendEnabled(bool enabled);
bool UsesForkBackend();

Ability_NativeChildProcess_ErrCode StartNativeChildProcess(
    const char* entry,
    NativeChildProcess_Args args,
    NativeChildProcess_Options options,
    int32_t* pid);

} // namespace winehua::ncp

#endif // WINEHUA_NCP_BACKEND_H
