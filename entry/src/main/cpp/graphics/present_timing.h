#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace winehua {

// Bounded, allocation-free collection. Serialization happens once per window,
// only in explicit observation mode. Preserve every interval, not a percentile
// of sparse 120-frame averages; automation can combine windows correctly.
class PresentTimingWindow {
public:
    static constexpr size_t kSize = 120;
    bool Add(uint64_t cpuUs, uint64_t timestampNs, uint64_t requestUs,
             uint64_t drawUs, uint64_t publishUs, uint64_t restoreUs)
    {
        if (count_ == kSize) ClearWindow();
        cpu_[count_] = cpuUs;
        interval_[count_] = lastTimestampNs_ && timestampNs > lastTimestampNs_
            ? (timestampNs - lastTimestampNs_) / 1000 : 0;
        lastTimestampNs_ = timestampNs;
        requestUs_ += requestUs;
        drawUs_ += drawUs;
        publishUs_ += publishUs;
        restoreUs_ += restoreUs;
        return ++count_ == kSize;
    }
    void Reset() { *this = {}; }
    size_t Count() const { return count_; }
    uint64_t RequestUs() const { return requestUs_; }
    uint64_t DrawUs() const { return drawUs_; }
    uint64_t PublishUs() const { return publishUs_; }
    uint64_t RestoreUs() const { return restoreUs_; }
    std::string CpuCsv() const { return Csv(cpu_); }
    std::string IntervalCsv() const { return Csv(interval_); }

private:
    void ClearWindow()
    {
        count_ = 0;
        requestUs_ = drawUs_ = publishUs_ = restoreUs_ = 0;
    }
    std::string Csv(const std::array<uint64_t, kSize>& values) const
    {
        std::string result;
        result.reserve(count_ * 7);
        for (size_t i = 0; i < count_; ++i) {
            if (i) result += ',';
            result += std::to_string(values[i]);
        }
        return result;
    }
    size_t count_ = 0;
    uint64_t lastTimestampNs_ = 0;
    uint64_t requestUs_ = 0, drawUs_ = 0, publishUs_ = 0, restoreUs_ = 0;
    std::array<uint64_t, kSize> cpu_{};
    std::array<uint64_t, kSize> interval_{};
};

} // namespace winehua
