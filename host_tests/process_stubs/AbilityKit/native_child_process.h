#pragma once
#include <cstdint>
constexpr int NCP_NO_ERROR = 0;
inline int OH_Ability_RegisterNativeChildProcessExitCallback(void (*)(int32_t, int32_t)) {
    return NCP_NO_ERROR;
}

int ProcessTestNcpKill(int32_t);
inline int OH_Ability_KillChildProcess(int32_t pid) { return ProcessTestNcpKill(pid); }
