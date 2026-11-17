#ifndef WINEHUA_AUDIO_BROKER_H
#define WINEHUA_AUDIO_BROKER_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include "protocols/audio_ipc_protocol.h"
#include "audio/audio_pcm_metrics.h"
#include "audio/audio_pcm_capture.h"
#include "audio/audio_stream.h"

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

    struct AudioRendererSlot
    {
        AudioBroker* broker = nullptr;
        uint32_t streamId = 0;
        uint32_t ordinal = 0;
        uint32_t gainQ15 = kUnityGainQ15;
        StreamPtr stream;
        OH_AudioRenderer* renderer = nullptr;
        uint32_t callbackFrames = 0;
        bool rendererStarted = false;
        std::atomic<bool> wantStarted{false};
        uint64_t primeDeadlineNs = 0;
        uint64_t fadeDeadlineNs = 0;
        std::atomic<int32_t> fadeDir{0};
        std::atomic<uint32_t> fadeRemaining{0};
        std::atomic<float> gain{1.0f};
        std::atomic<uint32_t> fadeRecoverCount{0};
        std::atomic<uint64_t> callbacks{0};
        std::atomic<uint64_t> requestedFrames{0};
        std::atomic<uint64_t> consumedFrames{0};
        std::atomic<uint64_t> underrunEvents{0};
        std::atomic<uint64_t> underrunFrames{0};
        std::atomic<uint32_t> minQueuedFrames{~0u};
        std::atomic<int32_t> peakAbs{0};
        std::atomic<uint64_t> lastCallbackNs{0};
        std::atomic<uint64_t> intervalMaxNs{0};
        std::atomic<uint64_t> lateCallbacks{0};
        std::atomic<int32_t> maxAdjacentDelta{0};
        std::atomic<uint64_t> pcmGeneration{1};
        std::atomic<uint64_t> callbackWallTotalNs{0};
        std::atomic<uint64_t> callbackWallMaxNs{0};
        std::atomic<uint64_t> ringReadTotalNs{0};
        std::atomic<uint64_t> metricsTotalNs{0};
        std::atomic<uint32_t> startBoundaryMaxL{0};
        std::atomic<uint32_t> startBoundaryMaxR{0};
        std::atomic<uint32_t> stopBoundaryMaxL{0};
        std::atomic<uint32_t> stopBoundaryMaxR{0};
        PcmContinuityState ringContinuity;
        PcmContinuityState outputContinuity;
        PcmMetricAccumulators ringMetrics;
        PcmMetricAccumulators outMetrics;
        PcmDiagnosticCapture ringCapture;
        PcmDiagnosticCapture outCapture;
        std::atomic<int32_t> lastValidL{0};
        std::atomic<int32_t> lastValidR{0};
        std::atomic<int32_t> lastValidBeforeStopL{0};
        std::atomic<int32_t> lastValidBeforeStopR{0};
        std::atomic<bool> haveLastValid{false};
        std::atomic<bool> haveLastValidBeforeStop{false};
        std::atomic<bool> expectFirstAfterStart{false};
        std::atomic<bool> sawStopEdge{false};
    };

    AudioBroker() = default;
    ~AudioBroker();
    AudioBroker(const AudioBroker&) = delete;
    AudioBroker& operator=(const AudioBroker&) = delete;

    AudioRendererSlot* CreateRendererSlotLocked(uint32_t streamId);
    void ReleaseRendererLocked(uint32_t streamId);
    int32_t StartRendererLocked(AudioRendererSlot* slot);
    void StopRendererPlaybackLocked(AudioRendererSlot* slot);
    void BeginFadeLocked(AudioRendererSlot* slot, int32_t dir);
    void PumpRendererLifecycleLocked(AudioRendererSlot* slot);
    bool EnsureCapturerLocked();
    void StopCapturerLocked();
    bool HasStartedCaptureStreamLocked() const;
    void PublishSnapshotsLocked();
    bool FillRendererCallback(AudioRendererSlot* slot,
                              int16_t* dst,
                              uint32_t frames,
                              uint64_t* ringReadNs,
                              uint64_t* metricsNs);
    void DistributeCaptureFramesS16(const std::shared_ptr<const StreamSnapshot>& snapshot,
                                    const int16_t* src, uint32_t frames);
    void TelemetryLoop();
    void LifecycleLoop();
    void NoteCallbackTiming(AudioRendererSlot* slot, uint32_t frames);
    void RequestLifecyclePump();
    void BumpPcmGeneration(AudioRendererSlot* slot);
    void NoteGetStatusLatency(uint64_t elapsedNs);
    void MaybeArmP2Locked();
    void FlushP2Captures();

    static OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer* renderer,
                                                    void* userData,
                                                    void* audioData,
                                                    int32_t audioDataSize);
    static void OnReadData(OH_AudioCapturer* capturer,
                           void* userData,
                           void* audioData,
                           int32_t audioDataSize);
    static void OnInterrupt(OH_AudioRenderer* renderer,
                            void* userData,
                            OH_AudioInterrupt_ForceType type,
                            OH_AudioInterrupt_Hint hint);
    static void OnOutputDeviceChange(OH_AudioRenderer* renderer,
                                      void* userData,
                                      OH_AudioStream_DeviceChangeReason reason);
    static void OnRendererError(OH_AudioRenderer* renderer,
                                 void* userData,
                                 OH_AudioStream_Result error);

    mutable std::mutex mutex_;
    std::condition_variable lifecycleCv_;
    bool running_ = false;
    std::string runtimeDir_;
    uint32_t nextStreamId_ = 1;
    uint32_t nextRenderOrdinal_ = 0;
    OH_AudioCapturer* capturer_ = nullptr;
    uint32_t capturerCallbackFrames_ = 0;
    bool capturerRunning_ = false;
    std::unique_ptr<AudioIpcServer> server_;
    std::unordered_map<uint32_t, StreamPtr> streams_;
    std::unordered_map<uint32_t, std::unique_ptr<AudioRendererSlot>> renderers_;
    std::shared_ptr<const StreamSnapshot> captureSnapshot_;

    std::atomic<uint32_t> telemetryRendererCreateFailures_{0};
    std::atomic<uint32_t> telemetryRendererStartFailures_{0};
    std::atomic<uint32_t> telemetryInterruptCount_{0};
    std::atomic<uint32_t> telemetryResumeCount_{0};
    std::atomic<uint32_t> telemetryRouteGeneration_{0};
    std::atomic<uint64_t> getStatusCalls_{0};
    std::atomic<uint64_t> getStatusLatencyTotalNs_{0};
    std::atomic<uint64_t> getStatusLatencyMaxNs_{0};
    std::atomic<uint32_t> physicalStartCount_{0};
    std::atomic<uint32_t> physicalStopCount_{0};
    std::atomic<uint64_t> physicalStartWallMaxNs_{0};
    std::atomic<uint64_t> physicalStopWallMaxNs_{0};
    uint32_t p2LastStartedCount_ = 0;
    uint32_t p2CapturesStarted_ = 0;
    std::atomic<bool> workerStop_{false};
    std::thread telemetryThread_;
    std::thread lifecycleThread_;
};

} // namespace winehua

#endif
