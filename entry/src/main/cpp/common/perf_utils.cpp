#include "perf_utils.h"
#include "displayed_fps.h"
#include "frame_loop_diagnostics.h"

#include <fcntl.h>
#include <unistd.h>

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_EGL"

namespace winehua {

void FrameLoopDiagnosticWindow::MaybePublish(uint32_t rendererId, uint32_t rootId, uint64_t nowUs)
{
    if (!Due(nowUs)) return;
    const auto source = gProducerDiagnostics.Snapshot();
    auto quantiles = [](const LoopTimingHistogram& histogram) {
        char value[96];
        std::snprintf(value, sizeof(value), "%llu/%llu/%llu/%llu",
            static_cast<unsigned long long>(histogram.UpperPercentile(50)),
            static_cast<unsigned long long>(histogram.UpperPercentile(95)),
            static_cast<unsigned long long>(histogram.UpperPercentile(99)),
            static_cast<unsigned long long>(histogram.maxUs));
        return std::string(value);
    };
    const uint64_t elapsedUs = nowUs - startedUs;
    char line[2048];
    std::snprintf(line, sizeof(line),
        "[FRAME-LOOP] renderer=%u root=%u window_us=%llu loops=%llu no_frame=%llu skipped=%llu "
        "presents=%llu failed=%llu fps=%.2f cpu_frames=%llu zc_frames=%llu direct_frames=%llu geometry_frames=%llu "
        "work_sum_us=%llu wait_sum_us=%llu take_us_le=%s work_us_le=%s wait_us_le=%s lock_us_le=%s gap_us_le=%s "
        "gap_samples=%llu idle_present_us=%llu vsync_requests=%llu vsync_timeouts=%llu vsync_errors=%llu fallbacks=%llu "
        "global_shm_top=%llu global_shm_sub=%llu global_other=%llu global_unmap=%llu "
        "global_shm_bytes=%llu global_commit_us=%llu global_callbacks=%llu",
        rendererId, rootId, (unsigned long long)elapsedUs, (unsigned long long)loops,
        (unsigned long long)noFrame, (unsigned long long)skipped, (unsigned long long)presents,
        (unsigned long long)failed, presents * 1000000.0 / elapsedUs,
        (unsigned long long)cpuFrames, (unsigned long long)zcFrames, (unsigned long long)directFrames,
        (unsigned long long)geometryFrames, (unsigned long long)work.sumUs, (unsigned long long)wait.sumUs,
        quantiles(take).c_str(), quantiles(work).c_str(), quantiles(wait).c_str(),
        quantiles(lockWait).c_str(), quantiles(presentGap).c_str(),
        (unsigned long long)presentGap.count,
        (unsigned long long)(lastPresentUs && nowUs >= lastPresentUs ? nowUs-lastPresentUs : elapsedUs),
        (unsigned long long)vsyncRequests, (unsigned long long)vsyncTimeouts,
        (unsigned long long)vsyncErrors, (unsigned long long)fallbacks,
        (unsigned long long)(source.shmTop-producerStart.shmTop),
        (unsigned long long)(source.shmSub-producerStart.shmSub),
        (unsigned long long)(source.other-producerStart.other),
        (unsigned long long)(source.unmap-producerStart.unmap),
        (unsigned long long)(source.bytes-producerStart.bytes),
        (unsigned long long)(source.commitUs-producerStart.commitUs),
        (unsigned long long)(source.callbacks-producerStart.callbacks));
    OH_LOG_INFO(LOG_APP, "%{public}s", line);
    ResetWindow(nowUs, source);
}

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

extern "C" int winehua_display_fps_publish(uint32_t id, uint64_t sequence, double fps,
                                           const char* route, uint64_t surfaceKey)
{
    (void)route; (void)surfaceKey;
    return PublishDisplayedFpsSample(kDisplayedFpsBasePath, id, sequence, fps,
                                     PerfNowUs(), true) ? 0 : -1;
}

void RendererPerfWindow::PublishDisplayedFps(uint32_t toplevelId, uint64_t nowUs)
{
    const uint64_t elapsedUs = nowUs - publishStartedUs;
    if (elapsedUs < 1000000) return;
    const double fps = static_cast<double>(publishFrames) * 1000000.0 /
                       static_cast<double>(std::max<uint64_t>(1, elapsedUs));
    if (PublishDisplayedFpsSample(kDisplayedFpsBasePath, toplevelId,
                                  publishSequence + 1, fps, nowUs))
        ++publishSequence;

    publishFrames = 0;
    publishStartedUs = nowUs;
}

void RendererPerfWindow::Add(uint32_t toplevelId, uint64_t take, uint64_t upload,
                             uint64_t swap, uint64_t total, size_t bytes, bool swapOk,
                             uint64_t calls, bool uploadTimed)
{
    if (publishToplevelId != toplevelId)
    {
        publishToplevelId = toplevelId;
        publishStartedUs = PerfNowUs();
        publishFrames = 0;
    }
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
    uploadCalls += calls;
    uploadTimedFrames += uploadTimed ? 1 : 0;
    if (!swapOk) ++failedSwaps;

    const uint64_t nowUs = PerfNowUs();
    PublishDisplayedFps(toplevelId, nowUs);

    if (count != kSamples) return;

    const double fps = static_cast<double>(windowDisplayed) * 1000000.0 /
                       static_cast<double>(std::max<uint64_t>(1, nowUs - startedUs));
    OH_LOG_INFO(LOG_APP,
                "[GL-PERF] tl=%{public}u displayed=%{public}llu fps=%{public}.2f "
                "upload_bytes=%{public}llu failed_swaps=%{public}llu "
                "upload_calls_issued=%{public}llu upload_timed_frames=%{public}llu "
                "take_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                "upload_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                "swap_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                "total_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu",
                toplevelId, static_cast<unsigned long long>(displayed), fps,
                static_cast<unsigned long long>(uploadBytes),
                static_cast<unsigned long long>(failedSwaps),
                static_cast<unsigned long long>(uploadCalls),
                static_cast<unsigned long long>(uploadTimedFrames),
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
    uploadCalls = 0;
    uploadTimedFrames = 0;
    failedSwaps = 0;
    startedUs = nowUs;
}

} // namespace winehua
