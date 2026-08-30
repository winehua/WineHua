#include "perf_utils.h"

#include <fcntl.h>
#include <unistd.h>

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_EGL"

namespace winehua {

uint64_t PerfNowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        PerfClock::now().time_since_epoch()).count());
}

uint64_t RendererPerfWindow::Percentile(std::array<uint64_t, kSamples> values, size_t count,
                                       unsigned int percentile)
{
    std::sort(values.begin(), values.begin() + count);
    const size_t index = std::min(count - 1, (count * percentile + 99) / 100 - 1);
    return values[index];
}

void RendererPerfWindow::PublishDisplayedFps(uint32_t toplevelId, uint64_t nowUs)
{
    static constexpr const char* kPath =
        "/data/storage/el2/base/files/.wine/drive_c/windows/temp/winehua_display_fps.txt";
    const uint64_t elapsedUs = nowUs - publishStartedUs;
    if (elapsedUs < 1000000) return;

    const double fps = static_cast<double>(publishFrames) * 1000000.0 /
                       static_cast<double>(std::max<uint64_t>(1, elapsedUs));
    char tempPath[192];
    char payload[128];
    const unsigned long long nextSequence =
        static_cast<unsigned long long>(publishSequence + 1);
    const int payloadLength = std::snprintf(
        payload, sizeof(payload), "%llu %.3f %u\n", nextSequence, fps, toplevelId);
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp.%d.%p",
                  kPath, getpid(), static_cast<void*>(this));

    const int fd = payloadLength > 0 && payloadLength < static_cast<int>(sizeof(payload))
        ? open(tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd >= 0)
    {
        const ssize_t written = write(fd, payload, static_cast<size_t>(payloadLength));
        close(fd);
        if (written == payloadLength && !rename(tempPath, kPath))
            publishSequence++;
        else
            unlink(tempPath);
    }

    publishFrames = 0;
    publishStartedUs = nowUs;
}

void RendererPerfWindow::Add(uint32_t toplevelId, uint64_t take, uint64_t upload,
                             uint64_t swap, uint64_t total, size_t bytes, bool swapOk)
{
    takeUs[count] = take;
    uploadUs[count] = upload;
    swapUs[count] = swap;
    totalUs[count] = total;
    ++count;
    if (swapOk)
    {
        ++displayed;
        ++windowDisplayed;
        ++publishFrames;
    }
    uploadBytes += bytes;
    if (!swapOk) ++failedSwaps;

    const uint64_t nowUs = PerfNowUs();
    PublishDisplayedFps(toplevelId, nowUs);

    if (count != kSamples) return;

    const double fps = static_cast<double>(windowDisplayed) * 1000000.0 /
                       static_cast<double>(std::max<uint64_t>(1, nowUs - startedUs));
    OH_LOG_INFO(LOG_APP,
                "[GL-PERF] tl=%{public}u displayed=%{public}llu fps=%{public}.2f "
                "upload_bytes=%{public}llu failed_swaps=%{public}llu "
                "take_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                "upload_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                "swap_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                "total_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu",
                toplevelId, static_cast<unsigned long long>(displayed), fps,
                static_cast<unsigned long long>(uploadBytes),
                static_cast<unsigned long long>(failedSwaps),
                static_cast<unsigned long long>(Percentile(takeUs, count, 50)),
                static_cast<unsigned long long>(Percentile(takeUs, count, 95)),
                static_cast<unsigned long long>(Percentile(takeUs, count, 99)),
                static_cast<unsigned long long>(*std::max_element(takeUs.begin(), takeUs.end())),
                static_cast<unsigned long long>(Percentile(uploadUs, count, 50)),
                static_cast<unsigned long long>(Percentile(uploadUs, count, 95)),
                static_cast<unsigned long long>(Percentile(uploadUs, count, 99)),
                static_cast<unsigned long long>(*std::max_element(uploadUs.begin(), uploadUs.end())),
                static_cast<unsigned long long>(Percentile(swapUs, count, 50)),
                static_cast<unsigned long long>(Percentile(swapUs, count, 95)),
                static_cast<unsigned long long>(Percentile(swapUs, count, 99)),
                static_cast<unsigned long long>(*std::max_element(swapUs.begin(), swapUs.end())),
                static_cast<unsigned long long>(Percentile(totalUs, count, 50)),
                static_cast<unsigned long long>(Percentile(totalUs, count, 95)),
                static_cast<unsigned long long>(Percentile(totalUs, count, 99)),
                static_cast<unsigned long long>(*std::max_element(totalUs.begin(), totalUs.end())));

    count = 0;
    windowDisplayed = 0;
    uploadBytes = 0;
    failedSwaps = 0;
    startedUs = nowUs;
}

} // namespace winehua
