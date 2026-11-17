#include "audio/audio_pcm_capture.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace winehua {

bool PcmDiagnosticCapture::Allocate(uint32_t capacityFrames)
{
    if (!capacityFrames)
        return false;
    if (buffer_ && capacityFrames_ == capacityFrames)
        return true;
    buffer_.reset(new (std::nothrow) int16_t[static_cast<size_t>(capacityFrames) * 2]);
    if (!buffer_)
    {
        capacityFrames_ = 0;
        return false;
    }
    capacityFrames_ = capacityFrames;
    writtenFrames_ = 0;
    droppedFrames_ = 0;
    captureId_ = 0;
    firstTimestampNs_ = 0;
    state_.store(State::Idle, std::memory_order_release);
    return true;
}

void PcmDiagnosticCapture::Arm(uint64_t captureId)
{
    const State current = state_.load(std::memory_order_acquire);
    if (current == State::Capturing || current == State::Ready)
        return;
    if (!buffer_ || !capacityFrames_)
        return;
    captureId_ = captureId;
    writtenFrames_ = 0;
    droppedFrames_ = 0;
    firstTimestampNs_ = 0;
    state_.store(State::Armed, std::memory_order_release);
}

void PcmDiagnosticCapture::CaptureFromCallback(const int16_t* pcm, uint32_t frames, uint64_t monotonicNs)
{
    State state = state_.load(std::memory_order_acquire);
    if (state == State::Armed)
    {
        if (state_.compare_exchange_strong(state, State::Capturing,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
        {
            firstTimestampNs_ = monotonicNs;
            writtenFrames_ = 0;
            droppedFrames_ = 0;
        }
        else if (state != State::Capturing)
        {
            return;
        }
    }
    else if (state != State::Capturing)
    {
        return;
    }

    if (!buffer_ || !pcm || !frames || !capacityFrames_)
        return;

    const uint32_t room = capacityFrames_ - writtenFrames_;
    const uint32_t take = std::min(frames, room);
    if (take)
    {
        std::memcpy(buffer_.get() + static_cast<size_t>(writtenFrames_) * 2,
                    pcm,
                    static_cast<size_t>(take) * 2 * sizeof(int16_t));
        writtenFrames_ += take;
    }
    if (take < frames)
        droppedFrames_ += frames - take;

    if (writtenFrames_ >= capacityFrames_ || take < frames)
        state_.store(State::Ready, std::memory_order_release);
}

bool PcmDiagnosticCapture::PeekReady(ReadyCapture* out) const
{
    if (!out)
        return false;
    if (state_.load(std::memory_order_acquire) != State::Ready)
        return false;
    out->captureId = captureId_;
    out->firstTimestampNs = firstTimestampNs_;
    out->frames = writtenFrames_;
    out->droppedFrames = droppedFrames_;
    out->samples = buffer_.get();
    return true;
}

void PcmDiagnosticCapture::ResetToIdle()
{
    state_.store(State::Idle, std::memory_order_release);
}

} // namespace winehua
