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

class AudioStream
{
public:
    AudioStream(uint32_t streamId,
                uint32_t ownerConnectionId,
                uint32_t ownerPid,
                std::string processName,
                const AudioStreamFormat& format,
                uint32_t ringCapacityFrames);
    ~AudioStream();

    bool Create();

    size_t ReadFrames(void* dst, size_t frames);
    void Reset();
    void SetStarted(bool started);
    void MarkClosed();
    void IncrementUnderrun();
    void IncrementOverflow();

    uint32_t stream_id() const { return streamId_; }
    uint32_t owner_connection_id() const { return ownerConnectionId_; }
    uint32_t owner_pid() const { return ownerPid_; }
    const std::string& process_name() const { return processName_; }
    const AudioStreamFormat& format() const { return format_; }

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
    int ringFd_ = -1;
    size_t ringMappingSize_ = 0;
    WinehuaAudioRingBuffer* ring_ = nullptr;
    std::atomic<uint32_t> readAttempts_{0};
    std::atomic<uint64_t> totalFramesRead_{0};
};

} // namespace winehua

#endif
