#ifndef WINEHUA_AUDIO_PCM_CAPTURE_H
#define WINEHUA_AUDIO_PCM_CAPTURE_H

#include <atomic>
#include <cstdint>
#include <memory>

namespace winehua {

// 2.0 s of 48 kHz stereo S16. Preallocate on the control thread.
constexpr uint32_t kPcmCaptureCapacityFrames = 96000;

class PcmDiagnosticCapture
{
public:
    enum class State : uint32_t
    {
        Idle = 0,
        Armed = 1,
        Capturing = 2,
        Ready = 3,
    };

    struct ReadyCapture
    {
        uint64_t captureId = 0;
        uint64_t firstTimestampNs = 0;
        uint32_t frames = 0;
        uint32_t droppedFrames = 0;
        const int16_t* samples = nullptr;
    };

    bool Allocate(uint32_t capacityFrames);
    void Arm(uint64_t captureId);
    void CaptureFromCallback(const int16_t* pcm, uint32_t frames, uint64_t monotonicNs);
    bool PeekReady(ReadyCapture* out) const;
    void ResetToIdle();

private:
    std::unique_ptr<int16_t[]> buffer_;
    uint32_t capacityFrames_ = 0;
    uint32_t writtenFrames_ = 0;
    uint32_t droppedFrames_ = 0;
    uint64_t captureId_ = 0;
    uint64_t firstTimestampNs_ = 0;
    std::atomic<State> state_{State::Idle};
};

} // namespace winehua

#endif
