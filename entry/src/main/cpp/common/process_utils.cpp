#include "process_utils.h"

#include "wait_utils.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

namespace winehua {

std::string DescribeWaitStatus(int status)
{
    if (WIFEXITED(status))
    {
        return "exited code=" + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status))
    {
        return "signaled signal=" + std::to_string(WTERMSIG(status));
    }
    return "status=" + std::to_string(status);
}

bool IsProcessRunningBySignal(pid_t pid)
{
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

void TerminateTrackedProcess(pid_t pid, bool usesNcp)
{
    if (pid <= 0) return;

    kill(pid, SIGTERM);
    if (usesNcp)
    {
        WaitFor("virgl native child exit", [pid]() { return !IsProcessRunningBySignal(pid); }, 2000, 100);
        if (IsProcessRunningBySignal(pid)) kill(pid, SIGKILL);
        return;
    }

    for (int i = 0; i < 20; ++i)
    {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) return;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

} // namespace winehua
