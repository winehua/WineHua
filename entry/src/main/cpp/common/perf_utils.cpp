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

/* 显示序列文件写入 —— 两条路线的宿主渲染器共用这一份 (X 路线见 display/ohos_output.c,
 * 声明见 common/display_fps.h)。原子 tmp+rename: guest 读到的永远是完整一行。
 * 返回 0 = 写入成功 (调用方据此决定序号是否推进, 与变更前语义一致)。 */
extern "C" int winehua_display_fps_publish(uint32_t surface_id, uint64_t sequence, double fps,
                                           const char *route, uint64_t surface_key)
{
    static constexpr const char* kPath =
        "/data/storage/el2/base/files/.wine/drive_c/windows/temp/winehua_display_fps.txt";
    char tempPath[192];
    char payload[160];
    const int payloadLength = std::snprintf(
        payload, sizeof(payload), "%llu %.3f %u %s %llu\n",
        static_cast<unsigned long long>(sequence), fps, surface_id,
        route && route[0] ? route : "-",
        static_cast<unsigned long long>(surface_key));
    /* 临时名必须逐次唯一: 同进程的发布者有多个 (X 路线合成器每秒一次 +
     * wayland 渲染器每个 toplevel 一个, 二者已确认同进程共存)。曾经用
     * pid+对象地址, 2026-10-01 改成仅 pid ⇒ 两个发布者可同时 O_TRUNC 同一个
     * 临时文件, guest 可能读到空行/半行, 或 rename 失败致序号不推进
     * (调用方按返回值决定是否推进) ⇒ displayed 门假失败。计数器保证唯一。 */
    static std::atomic<unsigned> tempSeq{0};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp.%d.%u", kPath, getpid(),
                  tempSeq.fetch_add(1, std::memory_order_relaxed));

    const int fd = payloadLength > 0 && payloadLength < static_cast<int>(sizeof(payload))
        ? open(tempPath, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd < 0) return -1;
    const ssize_t written = write(fd, payload, static_cast<size_t>(payloadLength));
    close(fd);
    if (written != payloadLength || rename(tempPath, kPath))
    {
        unlink(tempPath);
        return -1;
    }
    return 0;
}

void RendererPerfWindow::PublishDisplayedFps(uint32_t toplevelId, uint64_t nowUs)
{
    const uint64_t elapsedUs = nowUs - publishStartedUs;
    if (elapsedUs < 1000000) return;

    const double fps = static_cast<double>(publishFrames) * 1000000.0 /
                       static_cast<double>(std::max<uint64_t>(1, elapsedUs));
    /* 序号只在写入成功后前进 (guest 侧以"序号变化"判定宿主出图) */
    if (!winehua_display_fps_publish(toplevelId, publishSequence + 1, fps, "wayland",
                                     surfaceKey))
        publishSequence++;

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
