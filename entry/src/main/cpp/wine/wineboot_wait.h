#pragma once

#include "proc/wine_process.h"
#include <strings.h>
#include <utility>

namespace winehua {

struct WinebootAttemptProgress {
    bool launcherRunning = false;
    bool workerObserved = false;
    bool workerRunning = false;
    pid_t failedPid = 0;
    int exitCode = -1;
};

// The process registry retains exited children across engine restarts. Capture
// its identities before spawning, so a previous failure cannot poison a new
// attempt. This is only an observation of the existing registry, not a second
// process owner. Including start time also distinguishes a reused PID.
class WinebootAttempt {
public:
    explicit WinebootAttempt(const std::vector<WineProcessEntry>& before) {
        for (const auto& entry : before)
            previous_.emplace_back(entry.pid, entry.startTimestampMs);
    }

    WinebootAttemptProgress Inspect(const std::vector<WineProcessEntry>& entries,
                                   pid_t launcherPid) const {
        WinebootAttemptProgress result;
        for (const auto& entry : entries) {
            const bool launcher = entry.pid == launcherPid;
            const bool worker = !strcasecmp(entry.exeBasename.c_str(), "wineboot.exe");
            if ((!launcher && !worker) || WasPresent(entry)) continue;
            if (launcher) result.launcherRunning = entry.running;
            if (worker) {
                result.workerObserved = true;
                result.workerRunning = result.workerRunning || entry.running;
            }
            if (!entry.running && entry.exitCode > 0 && result.failedPid == 0) {
                result.failedPid = entry.pid;
                result.exitCode = entry.exitCode;
            }
        }
        return result;
    }

private:
    bool WasPresent(const WineProcessEntry& entry) const {
        for (const auto& identity : previous_) {
            if (identity.first == entry.pid && identity.second == entry.startTimestampMs)
                return true;
        }
        return false;
    }
    std::vector<std::pair<pid_t, uint64_t>> previous_;
};

} // namespace winehua
