#ifndef WINEHUA_AUDIO_BROKER_H
#define WINEHUA_AUDIO_BROKER_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ohaudio/native_audiostreambuilder.h>

#include "audio_ipc_protocol.h"
#include "audio_stream.h"

namespace winehua {

class AudioIpcServer;

class AudioBroker
{
public:
    static AudioBroker& GetInstance();

    bool EnsureStarted(const std::string& runtimeDir);
    void Stop();
    int CreateBootstrapHandle();

    int32_t RegisterClient(uint32_t connectionId,
                           const WinehuaAudioHelloReq& req,
                           WinehuaAudioHelloResp* resp);
    void CleanupClientStreams(uint32_t connectionId);

    int32_t OpenStream(uint32_t connectionId,
                       uint32_t clientPid,
                       const std::string& processName,
                       const WinehuaAudioOpenStreamReq& req,
                       WinehuaAudioOpenStreamResp* resp,
                       int* outRingFd);
    int32_t StartStream(uint32_t connectionId, uint32_t streamId);
    int32_t StopStream(uint32_t connectionId, uint32_t streamId);
    int32_t ResetStream(uint32_t connectionId, uint32_t streamId);
    int32_t CloseStream(uint32_t connectionId, uint32_t streamId);
    int32_t GetStatus(uint32_t connectionId, uint32_t streamId, WinehuaAudioGetStatusResp* resp);

private:
    using StreamPtr = std::shared_ptr<AudioStream>;
    using StreamSnapshot = std::vector<StreamPtr>;

    AudioBroker() = default;
    ~AudioBroker() = default;
    AudioBroker(const AudioBroker&) = delete;
    AudioBroker& operator=(const AudioBroker&) = delete;

    bool EnsureRendererLocked();
    void StopRendererLocked();
    void PublishSnapshotLocked();
    void MixStreamsS16(const std::shared_ptr<const StreamSnapshot>& snapshot, int16_t* dst, uint32_t frames);

    static OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer* renderer,
                                                    void* userData,
                                                    void* audioData,
                                                    int32_t audioDataSize);

    mutable std::mutex mutex_;
    bool running_ = false;
    std::string runtimeDir_;
    uint32_t nextStreamId_ = 1;
    OH_AudioRenderer* renderer_ = nullptr;
    uint32_t rendererCallbackFrames_ = 0;
    std::unique_ptr<AudioIpcServer> server_;
    std::unordered_map<uint32_t, StreamPtr> streams_;
    std::shared_ptr<const StreamSnapshot> renderSnapshot_;
    std::vector<int16_t> mixScratch_;
    std::vector<int32_t> mixAccum_;
};

} // namespace winehua

#endif
