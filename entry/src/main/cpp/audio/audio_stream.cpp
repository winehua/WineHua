#include "audio_stream.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace winehua {

AudioStream::AudioStream(uint32_t streamId,
                         uint32_t ownerConnectionId,
                         uint32_t ownerPid,
                         std::string processName,
                         const AudioStreamFormat& format,
                         uint32_t ringCapacityFrames,
                         AudioStreamDirection direction)
    : streamId_(streamId),
      ownerConnectionId_(ownerConnectionId),
      ownerPid_(ownerPid),
      processName_(std::move(processName)),
      format_(format),
      ringCapacityFrames_(ringCapacityFrames),
      direction_(direction)
{
}

AudioStream::~AudioStream()
{
    Close();
}

bool AudioStream::Create()
{
    char memfdName[64];

    std::snprintf(memfdName, sizeof(memfdName), "winehua-audio-%u", streamId_);
    ringMappingSize_ = winehua_audio_ring_header_size() +
                       static_cast<size_t>(ringCapacityFrames_) * format_.frameSize;

    ringFd_ = memfd_create(memfdName, MFD_CLOEXEC);
    if (ringFd_ < 0) return false;
    if (ftruncate(ringFd_, static_cast<off_t>(ringMappingSize_)) != 0) return false;

    ring_ = static_cast<WinehuaAudioRingBuffer*>(
        mmap(nullptr, ringMappingSize_, PROT_READ | PROT_WRITE, MAP_SHARED, ringFd_, 0));
    if (ring_ == MAP_FAILED)
    {
        ring_ = nullptr;
        return false;
    }

    std::memset(ring_, 0, ringMappingSize_);
    ring_->magic = WINEHUA_AUDIO_RING_MAGIC;
    ring_->version = WINEHUA_AUDIO_PROTOCOL_VERSION;
    ring_->sample_rate = format_.sampleRate;
    ring_->channels = format_.channels;
    ring_->sample_format = format_.sampleFormat;
    ring_->frame_size = format_.frameSize;
    ring_->capacity_frames = ringCapacityFrames_;
    winehua_audio_ring_reset(ring_, WINEHUA_AUDIO_STREAM_STOPPED);
    return true;
}

size_t AudioStream::ReadFrames(void* dst, size_t frames)
{
    size_t readFrames;

    if (!ring_) return 0;

    readAttempts_.fetch_add(1u, std::memory_order_relaxed);
    readFrames = winehua_audio_ring_read_frames(ring_, dst, frames);
    totalFramesRead_.fetch_add(static_cast<uint64_t>(readFrames), std::memory_order_relaxed);
    return readFrames;
}

size_t AudioStream::WriteFrames(const void* src, size_t frames)
{
    if (!ring_) return 0;
    return winehua_audio_ring_write_frames(ring_, src, frames);
}

void AudioStream::Reset()
{
    if (!ring_) return;
    winehua_audio_ring_reset(ring_, WINEHUA_AUDIO_STREAM_STOPPED);
    ClearHold();
}

void AudioStream::SetStarted(bool started)
{
    if (!ring_) return;
    winehua_audio_ring_store_state(
        ring_, started ? WINEHUA_AUDIO_STREAM_STARTED : WINEHUA_AUDIO_STREAM_STOPPED);
    winehua_audio_ring_increment_seq(ring_);
    if (!started) ClearHold();
}

void AudioStream::MarkClosed()
{
    if (!ring_) return;
    winehua_audio_ring_store_state(ring_, WINEHUA_AUDIO_STREAM_CLOSED);
    winehua_audio_ring_increment_seq(ring_);
}

void AudioStream::IncrementUnderrun()
{
    if (ring_) winehua_audio_ring_increment_underrun(ring_);
}

void AudioStream::IncrementOverflow()
{
    if (ring_) winehua_audio_ring_increment_overflow(ring_);
}

void AudioStream::NoteLastFrame(const int16_t* samples, size_t frames)
{
    if (!samples || !frames) return;
    lastL_ = samples[(frames - 1) * 2];
    lastR_ = samples[(frames - 1) * 2 + 1];
    haveLast_ = true;
}

void AudioStream::HoldPadRemainder(int16_t* samples, size_t gotFrames, uint32_t frames)
{
    if (!samples || gotFrames >= frames) return;
    const int16_t holdL = haveLast_ ? lastL_ : 0;
    const int16_t holdR = haveLast_ ? lastR_ : 0;
    for (size_t i = gotFrames; i < frames; ++i)
    {
        samples[i * 2] = holdL;
        samples[i * 2 + 1] = holdR;
    }
}

void AudioStream::ClearHold()
{
    lastL_ = 0;
    lastR_ = 0;
    haveLast_ = false;
    consecutiveUnderruns_ = 0;
}

void AudioStream::MarkMixUnderrun()
{
    IncrementUnderrun();
    consecutiveUnderruns_++;
}

void AudioStream::MarkMixFilled()
{
    consecutiveUnderruns_ = 0;
}

bool AudioStream::started() const
{
    return state() == WINEHUA_AUDIO_STREAM_STARTED;
}

uint32_t AudioStream::state() const
{
    return ring_ ? winehua_audio_ring_load_state(ring_) : WINEHUA_AUDIO_STREAM_CLOSED;
}

uint32_t AudioStream::queued_frames() const
{
    return ring_ ? winehua_audio_ring_used_frames(ring_) : 0;
}

uint32_t AudioStream::free_frames() const
{
    return ring_ ? winehua_audio_ring_free_frames(ring_) : 0;
}

uint32_t AudioStream::underrun_count() const
{
    return ring_ ? winehua_audio_ring_load_underrun_count(ring_) : 0;
}

uint32_t AudioStream::overflow_count() const
{
    return ring_ ? winehua_audio_ring_load_overflow_count(ring_) : 0;
}

uint32_t AudioStream::read_attempts() const
{
    return readAttempts_.load(std::memory_order_relaxed);
}

uint64_t AudioStream::total_frames_read() const
{
    return totalFramesRead_.load(std::memory_order_relaxed);
}

void AudioStream::Close()
{
    if (ring_)
    {
        munmap(ring_, ringMappingSize_);
        ring_ = nullptr;
    }
    if (ringFd_ >= 0)
    {
        close(ringFd_);
        ringFd_ = -1;
    }
}

} // namespace winehua
