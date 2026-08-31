#pragma once

#include <string>
#include <sys/types.h>

namespace winehua {

std::string DescribeWaitStatus(int status);
bool IsProcessRunningBySignal(pid_t pid);
void TerminateTrackedProcess(pid_t pid, bool usesNcp);

} // namespace winehua
