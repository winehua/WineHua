#ifndef WINEHUA_AUDIO_STREAM_H
#define WINEHUA_AUDIO_STREAM_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "audio_ipc_protocol.h"

namespace winehua {

struct AudioStreamFormat
{
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t sampleFormat = 0;
    uint32_t frameSize = 0;
    uint32_t preferredPeriodFrames = 0;
};

enum class AudioStreamDirection
{
    Render,
    Capture,
};

class AudioStream
{
public:
    AudioStream(uint32_t streamId,
                uint32_t ownerConnectionId,
                uint32_t ownerPid,
                std::string processName,
                const AudioStreamFormat& format,
                uint32_t ringCapacityFrames,
                AudioStreamDirection direction);
    ~AudioStream();

    bool Create();

    size_t ReadFrames(void* dst, size_t frames);
    size_t WriteFrames(const void* src, size_t frames);
    void Reset();
    void SetStarted(bool started);
    void MarkClosed();
    void IncrementUnderrun();
    void IncrementOverflow();
    void NoteLastFrame(const int16_t* samples, size_t frames);
    void HoldPadRemainder(int16_t* samples, size_t gotFrames, uint32_t frames);
    void MarkMixUnderrun();
    void MarkMixFilled();
    uint32_t consecutive_underruns() const { return consecutiveUnderruns_; }
    bool has_hold() const { return haveLast_; }
    void ClearHold();

    uint32_t stream_id() const { return streamId_; }
    uint32_t owner_connection_id() const { return ownerConnectionId_; }
    uint32_t owner_pid() const { return ownerPid_; }
    const std::string& process_name() const { return processName_; }
    const AudioStreamFormat& format() const { return format_; }
    AudioStreamDirection direction() const { return direction_; }

    bool started() const;
    uint32_t state() const;
    uint32_t ring_capacity_frames() const { return ringCapacityFrames_; }
    int ring_fd() const { return ringFd_; }
    size_t ring_mapping_size() const { return ringMappingSize_; }
    uint32_t queued_frames() const;
    uint32_t free_frames() const;
    uint32_t underrun_count() const;
    uint32_t overflow_count() const;
    uint32_t read_attempts() const;
    uint64_t total_frames_read() const;

private:
    void Close();

    uint32_t streamId_;
    uint32_t ownerConnectionId_;
    uint32_t ownerPid_;
    std::string processName_;
    AudioStreamFormat format_;
    uint32_t ringCapacityFrames_;
    AudioStreamDirection direction_;
    int ringFd_ = -1;
    size_t ringMappingSize_ = 0;
    WinehuaAudioRingBuffer* ring_ = nullptr;
    std::atomic<uint32_t> readAttempts_{0};
    std::atomic<uint64_t> totalFramesRead_{0};
    int16_t lastL_ = 0;
    int16_t lastR_ = 0;
    bool haveLast_ = false;
    uint32_t consecutiveUnderruns_ = 0;
};

} // namespace winehua

#endif
