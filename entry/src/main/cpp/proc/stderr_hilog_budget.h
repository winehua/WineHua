#pragma once

#include <cstdint>
#include <cstring>

namespace winehua {

// Bound the system-log mirror, never the raw stderr file or Wine's fault handler.
// One reader owns this state. Crash/error lines have a separate budget so an
// SMC trace flood cannot consume their allowance. Full forwarding is opt-in.
class StderrHilogBudget {
public:
    explicit StderrHilogBudget(bool full = false) : full_(full) {}

    uint64_t BeginSecond(uint64_t second) {
        if (started_ && second == second_) return 0;
        const uint64_t skipped = skipped_;
        second_ = second;
        started_ = true;
        normal_ = critical_ = 0;
        skipped_ = 0;
        return skipped;
    }

    bool Allow(const char* line) {
        if (full_) return true;
        const bool critical = IsCritical(line);
        unsigned& count = critical ? critical_ : normal_;
        const unsigned limit = critical ? 8 : 32;
        if (count < limit) { ++count; return true; }
        ++skipped_;
        return false;
    }

    uint64_t Skipped() const { return skipped_; }

private:
    static bool IsCritical(const char* line) {
        return std::strstr(line, "err:") || std::strstr(line, "Unhandled exception") ||
               std::strstr(line, "unhandled exception") || std::strstr(line, "[early-fault]") ||
               std::strstr(line, "[FAULT-MIN]") || std::strstr(line, "[QUIT-PROBE]") ||
               std::strstr(line, "[EXIT-FAULT]") || std::strstr(line, "Assertion") ||
               std::strstr(line, "FATAL");
    }
    bool full_ = false, started_ = false;
    uint64_t second_ = 0, skipped_ = 0;
    unsigned normal_ = 0, critical_ = 0;
};

} // namespace winehua
