#include "audio/audio_broker.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_AUDIO"
#include <hilog/log.h>

#include "audio/audio_diag_config.h"
#include "audio/audio_ipc_server.h"
#include "audio/audio_pcm_capture.h"
#include "audio/audio_pcm_metrics.h"
#include "common/app_log.h"
#include "common/ring_buffer.h"

#include <cstdio>
#include <vector>

namespace winehua {

namespace {

constexpr uint32_t kAudioPrimePeriods = 2;
constexpr uint32_t kAudioRampFrames = 144;
constexpr uint64_t kAudioPrimeTimeoutNs = 80000000ull;
constexpr uint64_t kAudioFadeTimeoutNs = 40000000ull;

bool EnsureDir(const std::string& path)
{
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    return false;
}

void LogStreamStats(const char* reason, const std::shared_ptr<AudioStream>& stream)
{
    if (!stream) return;

    OH_LOG_INFO(
        LOG_APP,
        "[AudioBroker] %{public}s stream id=%{public}u conn=%{public}u pid=%{public}u queued=%{public}u free=%{public}u underrun=%{public}u overflow=%{public}u readCalls=%{public}u readFrames=%{public}llu state=%{public}u",
        reason,
        stream->stream_id(),
        stream->owner_connection_id(),
        stream->owner_pid(),
        stream->queued_frames(),
        stream->free_frames(),
        stream->underrun_count(),
        stream->overflow_count(),
        stream->read_attempts(),
        static_cast<unsigned long long>(stream->total_frames_read()),
        stream->state());
}

int32_t ValidateStreamOwner(const std::shared_ptr<AudioStream>& stream,
                            uint32_t connectionId,
                            uint32_t streamId)
{
    if (!stream) return -ENOENT;
    if (stream->owner_connection_id() == connectionId) return 0;

    OH_LOG_WARN(LOG_APP,
                "[AudioBroker] reject foreign stream access conn=%{public}u stream=%{public}u ownerConn=%{public}u",
                connectionId, streamId, stream->owner_connection_id());
    return -EPERM;
}

uint64_t MonotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t MonotonicUs()
{
    return MonotonicNs() / 1000ull;
}

const char* LogicalRenderName(bool want, bool physical, bool wineStarted, int32_t fadeDir)
{
    if (want && physical && wineStarted)
        return "RUNNING";
    if (want && !physical)
        return "PRIMING";
    if (!want && fadeDir < 0)
        return "FADING_OUT";
    if (wineStarted)
        return "LOGICAL";
    return "STOPPED";
}

const char* PhysicalRenderName(bool physical)
{
    return physical ? "STARTED" : "STOPPED";
}

void NotePeak(std::atomic<int32_t>* dst, int peak)
{
    int32_t prev = dst->load(std::memory_order_relaxed);
    while (peak > prev &&
           !dst->compare_exchange_weak(prev, peak, std::memory_order_relaxed))
    {
    }
}

void NoteMinQueued(std::atomic<uint32_t>* dst, uint32_t queued)
{
    uint32_t prev = dst->load(std::memory_order_relaxed);
    while (queued < prev &&
           !dst->compare_exchange_weak(prev, queued, std::memory_order_relaxed))
    {
    }
}

int16_t ScaleS16(int16_t sample, float gain)
{
    const float y = static_cast<float>(sample) * gain;
    if (y > 32767.0f) return 32767;
    if (y < -32768.0f) return static_cast<int16_t>(-32768);
    return static_cast<int16_t>(y);
}

void ApplyLinearRampS16(int16_t* samples, uint32_t frames, float* gain, float target, uint32_t* remaining)
{
    if (!samples || !frames || !gain || !remaining) return;

    uint32_t left = *remaining;
    float g = *gain;
    uint32_t rampFrames = left < frames ? left : frames;

    if (left && rampFrames)
    {
        const float step = (target - g) / static_cast<float>(left);
        for (uint32_t i = 0; i < rampFrames; ++i)
        {
            g += step;
            samples[i * 2] = ScaleS16(samples[i * 2], g);
            samples[i * 2 + 1] = ScaleS16(samples[i * 2 + 1], g);
        }
        left -= rampFrames;
        if (!left) g = target;
    }

    if (!left && target == 1.0f && g != 1.0f)
    {
        // Fade-in remaining was lost while gain was still 0. Do not mute a
        // running stream: pass PCM through and restore unity gain.
        *gain = 1.0f;
        *remaining = 0;
        return;
    }

    if (g != 1.0f)
    {
        for (uint32_t i = rampFrames; i < frames; ++i)
        {
            samples[i * 2] = ScaleS16(samples[i * 2], g);
            samples[i * 2 + 1] = ScaleS16(samples[i * 2 + 1], g);
        }
    }

    *gain = g;
    *remaining = left;
}

} // namespace

AudioBroker& AudioBroker::GetInstance()
{
    static AudioBroker broker;
    return broker;
}

AudioBroker::~AudioBroker()
{
    Stop();
}

void AudioBroker::RequestLifecyclePump()
{
    lifecycleCv_.notify_all();
}

void AudioBroker::BumpPcmGeneration(AudioRendererSlot* slot)
{
    if (!slot)
        return;
    slot->pcmGeneration.fetch_add(1, std::memory_order_relaxed);
}

void AudioBroker::NoteGetStatusLatency(uint64_t elapsedNs)
{
    getStatusCalls_.fetch_add(1, std::memory_order_relaxed);
    getStatusLatencyTotalNs_.fetch_add(elapsedNs, std::memory_order_relaxed);
    AtomicMax(getStatusLatencyMaxNs_, elapsedNs);
}

bool AudioBroker::EnsureStarted(const std::string& runtimeDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) return true;

    runtimeDir_ = runtimeDir + "/audio";
    if (!EnsureDir(runtimeDir) || !EnsureDir(runtimeDir_))
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to prepare runtime dir %{public}s", runtimeDir_.c_str());
        return false;
    }

    server_ = std::make_unique<AudioIpcServer>();
    if (!server_->Start(this))
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start bootstrap server");
        server_.reset();
        return false;
    }

    running_ = true;
    workerStop_.store(false, std::memory_order_relaxed);
    if (!telemetryThread_.joinable())
        telemetryThread_ = std::thread([this] { TelemetryLoop(); });
    if (!lifecycleThread_.joinable())
        lifecycleThread_ = std::thread([this] { LifecycleLoop(); });
    OH_LOG_INFO(LOG_APP,
                "[AudioBroker] started runtimeDir=%{public}s mix=48k/s16/2 per-endpoint B3 prime=%{public}u rampFrames=%{public}u",
                runtimeDir_.c_str(), kAudioPrimePeriods, kAudioRampFrames);
    OH_LOG_INFO(LOG_APP,
                "[AudioB3] started monoUs=%{public}llu commit=%{public}s profile=%{public}s b2=%{public}d p2=%{public}d mix=48k/s16/2 prime=%{public}u rampFrames=%{public}u",
                static_cast<unsigned long long>(MonotonicUs()),
                kAudioDiagGitCommit,
                AudioDiagGainProfileName(kAudioDiagGainProfile),
                kKeepRendererPhysicallyStartedForB2 ? 1 : 0,
                kAudioDiagP2Capture ? 1 : 0,
                kAudioPrimePeriods,
                kAudioRampFrames);
    return true;
}

void AudioBroker::Stop()
{
    workerStop_.store(true, std::memory_order_relaxed);
    lifecycleCv_.notify_all();
    if (telemetryThread_.joinable()) telemetryThread_.join();
    if (lifecycleThread_.joinable()) lifecycleThread_.join();

    std::unique_ptr<AudioIpcServer> server;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) return;

        running_ = false;
        server = std::move(server_);
    }

    if (server) server->Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> rendererIds;
    rendererIds.reserve(renderers_.size());
    for (const auto& [streamId, slot] : renderers_)
        rendererIds.push_back(streamId);
    for (uint32_t streamId : rendererIds)
        ReleaseRendererLocked(streamId);

    for (auto& [streamId, stream] : streams_)
    {
        LogStreamStats("broker-stop", stream);
        stream->MarkClosed();
    }
    streams_.clear();
    PublishSnapshotsLocked();
    StopCapturerLocked();
    OH_LOG_INFO(LOG_APP, "[AudioBroker] stopped");
}

int AudioBroker::CreateBootstrapHandle()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_ || !server_) return -1;
    return server_->CreateBootstrapHandle();
}

int32_t AudioBroker::RegisterClient(uint32_t connectionId,
                                    const WinehuaAudioHelloReq& req,
                                    WinehuaAudioHelloResp* resp)
{
    if (!resp) return -EINVAL;
    (void)connectionId;
    (void)req;

    std::memset(resp, 0, sizeof(*resp));
    resp->result = 0;
    resp->protocol_version = WINEHUA_AUDIO_PROTOCOL_VERSION;
    resp->mix_sample_rate = kAudioMixSampleRate;
    resp->mix_channels = kAudioMixChannels;
    resp->mix_sample_format = WINEHUA_AUDIO_SAMPLE_S16LE;
    return 0;
}

void AudioBroker::CleanupClientStreams(uint32_t connectionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;

    for (auto it = streams_.begin(); it != streams_.end();)
    {
        if (it->second->owner_connection_id() == connectionId)
        {
            LogStreamStats("cleanup", it->second);
            ReleaseRendererLocked(it->first);
            it->second->MarkClosed();
            it = streams_.erase(it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }

    if (changed)
    {
        PublishSnapshotsLocked();
        if (!HasStartedCaptureStreamLocked() && capturerRunning_ && capturer_)
            OH_AudioCapturer_Stop(capturer_);
        if (!HasStartedCaptureStreamLocked()) capturerRunning_ = false;
    }
}

int32_t AudioBroker::OpenStream(uint32_t connectionId,
                                uint32_t clientPid,
                                const std::string& processName,
                                const WinehuaAudioOpenStreamReq& req,
                                WinehuaAudioOpenStreamResp* resp,
                                int* outRingFd)
{
    std::lock_guard<std::mutex> lock(mutex_);
    AudioStreamFormat format;
    StreamPtr stream;
    uint32_t streamId;
    AudioRendererSlot* slot = nullptr;

    if (!resp || !outRingFd) return -EINVAL;
    std::memset(resp, 0, sizeof(*resp));
    *outRingFd = -1;

    if (!running_) return -EIO;
    if (req.sample_rate != kAudioMixSampleRate || req.channels != kAudioMixChannels ||
        req.sample_format != WINEHUA_AUDIO_SAMPLE_S16LE)
        return -EINVAL;

    const bool isCapture = (req.flags & WINEHUA_AUDIO_STREAM_FLAG_CAPTURE) != 0;
    if (isCapture)
    {
        if (!EnsureCapturerLocked()) return -EIO;
    }

    streamId = nextStreamId_++;
    format.sampleRate = kAudioMixSampleRate;
    format.channels = kAudioMixChannels;
    format.sampleFormat = WINEHUA_AUDIO_SAMPLE_S16LE;
    format.frameSize = kAudioMixFrameSize;
    format.preferredPeriodFrames = isCapture
        ? (capturerCallbackFrames_ ? capturerCallbackFrames_ : kAudioTargetCallbackFrames)
        : kAudioTargetCallbackFrames;

    if (!isCapture)
    {
        slot = CreateRendererSlotLocked(streamId);
        if (!slot)
        {
            OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create renderer for stream=%{public}u", streamId);
            return -EIO;
        }
        if (slot->callbackFrames)
            format.preferredPeriodFrames = slot->callbackFrames;
    }

    stream = std::make_shared<AudioStream>(
        streamId, connectionId, clientPid, processName, format,
        ComputeRingCapacityFrames(format.preferredPeriodFrames),
        isCapture ? AudioStreamDirection::Capture : AudioStreamDirection::Render);
    if (!stream->Create())
    {
        if (slot) ReleaseRendererLocked(streamId);
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create ring for stream=%{public}u", streamId);
        return -EIO;
    }

    if (slot) slot->stream = stream;

    resp->result = 0;
    resp->stream_id = streamId;
    resp->mix_sample_rate = kAudioMixSampleRate;
    resp->mix_channels = kAudioMixChannels;
    resp->mix_sample_format = WINEHUA_AUDIO_SAMPLE_S16LE;
    resp->mix_frame_size = kAudioMixFrameSize;
    resp->ring_capacity_frames = stream->ring_capacity_frames();
    resp->ring_mapping_size = static_cast<uint32_t>(stream->ring_mapping_size());
    resp->preferred_period_frames = format.preferredPeriodFrames;
    *outRingFd = stream->ring_fd();

    OH_LOG_INFO(LOG_APP,
                "[AudioBroker] open stream id=%{public}u conn=%{public}u pid=%{public}u name=%{public}s ringFrames=%{public}u period=%{public}u renderer=%{public}s",
                streamId, connectionId, clientPid, processName.c_str(),
                resp->ring_capacity_frames, resp->preferred_period_frames,
                isCapture ? "capture" : "endpoint");
    if (slot)
    {
        OH_LOG_INFO(LOG_APP,
                    "[AudioB3] open monoUs=%{public}llu commit=%{public}s profile=%{public}s stream=%{public}u ordinal=%{public}u gen=%{public}llu gainQ15=%{public}u callbackFrames=%{public}u rate=48000 ch=2 fmt=S16",
                    static_cast<unsigned long long>(MonotonicUs()),
                    kAudioDiagGitCommit,
                    AudioDiagGainProfileName(kAudioDiagGainProfile),
                    streamId,
                    slot->ordinal,
                    static_cast<unsigned long long>(slot->pcmGeneration.load(std::memory_order_relaxed)),
                    slot->gainQ15,
                    slot->callbackFrames);
    }

    streams_.emplace(streamId, stream);
    PublishSnapshotsLocked();
    return 0;
}

int32_t AudioBroker::StartStream(uint32_t connectionId, uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    int32_t ownerStatus = ValidateStreamOwner(it == streams_.end() ? nullptr : it->second,
                                              connectionId, streamId);

    if (ownerStatus != 0) return ownerStatus;
    it->second->SetStarted(true);

    if (it->second->direction() == AudioStreamDirection::Capture && capturer_)
    {
        if (!capturerRunning_)
        {
            if (OH_AudioCapturer_Start(capturer_) != AUDIOSTREAM_SUCCESS)
            {
                it->second->SetStarted(false);
                OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start capturer for stream id=%{public}u", streamId);
                return -EIO;
            }
            capturerRunning_ = true;
        }
    }
    else if (it->second->direction() == AudioStreamDirection::Render)
    {
        auto rendererIt = renderers_.find(streamId);
        AudioRendererSlot* slot = rendererIt == renderers_.end() ? nullptr : rendererIt->second.get();
        if (!slot || !slot->renderer)
        {
            it->second->SetStarted(false);
            return -EIO;
        }
        slot->wantStarted.store(true, std::memory_order_relaxed);
        slot->primeDeadlineNs = MonotonicNs() + kAudioPrimeTimeoutNs;
        slot->expectFirstAfterStart.store(true, std::memory_order_relaxed);
        slot->sawStopEdge.store(false, std::memory_order_relaxed);
        if (slot->rendererStarted)
        {
            BeginFadeLocked(slot, 1);
        }
        else
        {
            slot->fadeDir.store(0, std::memory_order_relaxed);
            slot->fadeRemaining.store(0, std::memory_order_relaxed);
            if (slot->gain.load(std::memory_order_relaxed) == 0.0f)
                slot->gain.store(1.0f, std::memory_order_relaxed);
        }
        PumpRendererLifecycleLocked(slot);
        RequestLifecyclePump();
        OH_LOG_INFO(LOG_APP,
                    "[AudioBroker] start stream id=%{public}u queued=%{public}u rendererStarted=%{public}d",
                    streamId, slot->stream ? slot->stream->queued_frames() : 0,
                    slot->rendererStarted ? 1 : 0);
        OH_LOG_INFO(LOG_APP,
                    "[AudioB3] logicalStart monoUs=%{public}llu stream=%{public}u ordinal=%{public}u gen=%{public}llu logical=%{public}s physical=%{public}s queued=%{public}u gainQ15=%{public}u",
                    static_cast<unsigned long long>(MonotonicUs()),
                    streamId,
                    slot->ordinal,
                    static_cast<unsigned long long>(slot->pcmGeneration.load(std::memory_order_relaxed)),
                    LogicalRenderName(true, slot->rendererStarted, true, 0),
                    PhysicalRenderName(slot->rendererStarted),
                    slot->stream ? slot->stream->queued_frames() : 0,
                    slot->gainQ15);
        MaybeArmP2Locked();
        return 0;
    }

    OH_LOG_INFO(LOG_APP, "[AudioBroker] start stream id=%{public}u", streamId);
    return 0;
}

int32_t AudioBroker::StopStream(uint32_t connectionId, uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    int32_t ownerStatus = ValidateStreamOwner(it == streams_.end() ? nullptr : it->second,
                                              connectionId, streamId);

    if (ownerStatus != 0) return ownerStatus;

    if (it->second->direction() == AudioStreamDirection::Capture)
    {
        it->second->SetStarted(false);
        if (capturerRunning_ && capturer_ && !HasStartedCaptureStreamLocked())
        {
            OH_AudioCapturer_Stop(capturer_);
            capturerRunning_ = false;
        }
    }
    else if (it->second->direction() == AudioStreamDirection::Render)
    {
        auto rendererIt = renderers_.find(streamId);
        AudioRendererSlot* slot = rendererIt == renderers_.end() ? nullptr : rendererIt->second.get();
        if (slot)
        {
            slot->wantStarted.store(false, std::memory_order_relaxed);
            if (slot->haveLastValid.load(std::memory_order_relaxed))
            {
                slot->lastValidBeforeStopL.store(slot->lastValidL.load(std::memory_order_relaxed),
                                                std::memory_order_relaxed);
                slot->lastValidBeforeStopR.store(slot->lastValidR.load(std::memory_order_relaxed),
                                                std::memory_order_relaxed);
                slot->haveLastValidBeforeStop.store(true, std::memory_order_relaxed);
            }
            slot->sawStopEdge.store(slot->rendererStarted, std::memory_order_relaxed);
            if (slot->rendererStarted)
                BeginFadeLocked(slot, -1);
            it->second->SetStarted(false);
            PumpRendererLifecycleLocked(slot);
            RequestLifecyclePump();
            OH_LOG_INFO(LOG_APP,
                        "[AudioB3] logicalStop monoUs=%{public}llu stream=%{public}u ordinal=%{public}u lastValidL=%{public}d lastValidR=%{public}d physical=%{public}s fade=%{public}d",
                        static_cast<unsigned long long>(MonotonicUs()),
                        streamId,
                        slot->ordinal,
                        static_cast<int>(slot->lastValidBeforeStopL.load(std::memory_order_relaxed)),
                        static_cast<int>(slot->lastValidBeforeStopR.load(std::memory_order_relaxed)),
                        PhysicalRenderName(slot->rendererStarted),
                        slot->fadeDir.load(std::memory_order_relaxed));
        }
        else
        {
            it->second->SetStarted(false);
        }
    }
    else
    {
        it->second->SetStarted(false);
    }

    OH_LOG_INFO(LOG_APP, "[AudioBroker] stop stream id=%{public}u", streamId);
    MaybeArmP2Locked();
    return 0;
}

int32_t AudioBroker::ResetStream(uint32_t connectionId, uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    int32_t ownerStatus = ValidateStreamOwner(it == streams_.end() ? nullptr : it->second,
                                              connectionId, streamId);

    if (ownerStatus != 0) return ownerStatus;
    auto rendererIt = renderers_.find(streamId);
    if (rendererIt != renderers_.end() && rendererIt->second)
    {
        AudioRendererSlot* slot = rendererIt->second.get();
        slot->wantStarted.store(false, std::memory_order_relaxed);
        slot->fadeDir.store(0, std::memory_order_relaxed);
        slot->fadeRemaining.store(0, std::memory_order_relaxed);
        slot->gain.store(1.0f, std::memory_order_relaxed);
        BumpPcmGeneration(slot);
        StopRendererPlaybackLocked(slot);
        if (slot->renderer) OH_AudioRenderer_Flush(slot->renderer);
    }
    it->second->Reset();
    if (it->second->direction() == AudioStreamDirection::Capture && capturerRunning_ && capturer_)
        OH_AudioCapturer_Flush(capturer_);
    return 0;
}

int32_t AudioBroker::CloseStream(uint32_t connectionId, uint32_t streamId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    int32_t ownerStatus = ValidateStreamOwner(it == streams_.end() ? nullptr : it->second,
                                              connectionId, streamId);

    if (ownerStatus != 0) return ownerStatus;
    LogStreamStats("close", it->second);
    const bool wasCapture = it->second->direction() == AudioStreamDirection::Capture;
    ReleaseRendererLocked(streamId);
    it->second->MarkClosed();
    streams_.erase(it);
    PublishSnapshotsLocked();
    if (wasCapture && capturerRunning_ && capturer_ && !HasStartedCaptureStreamLocked())
    {
        OH_AudioCapturer_Stop(capturer_);
        capturerRunning_ = false;
    }
    return 0;
}

int32_t AudioBroker::GetStatus(uint32_t connectionId, uint32_t streamId, WinehuaAudioGetStatusResp* resp)
{
    const uint64_t t0 = MonotonicNs();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    int32_t ownerStatus = ValidateStreamOwner(it == streams_.end() ? nullptr : it->second,
                                              connectionId, streamId);

    if (!resp)
    {
        NoteGetStatusLatency(MonotonicNs() - t0);
        return -EINVAL;
    }
    std::memset(resp, 0, sizeof(*resp));
    if (ownerStatus != 0)
    {
        NoteGetStatusLatency(MonotonicNs() - t0);
        return ownerStatus;
    }

    resp->result = 0;
    resp->stream_id = streamId;
    resp->state = it->second->state();
    resp->queued_frames = it->second->queued_frames();
    resp->free_frames = it->second->free_frames();
    resp->underrun_count = it->second->underrun_count();
    resp->overflow_count = it->second->overflow_count();
    NoteGetStatusLatency(MonotonicNs() - t0);
    return 0;
}

AudioBroker::AudioRendererSlot* AudioBroker::CreateRendererSlotLocked(uint32_t streamId)
{
    OH_AudioStreamBuilder* builder = nullptr;
    OH_AudioStream_Result result = AUDIOSTREAM_SUCCESS;
    int32_t callbackFrames = 0;
    auto slot = std::make_unique<AudioRendererSlot>();

    slot->broker = this;
    slot->streamId = streamId;
    slot->ordinal = nextRenderOrdinal_++;
    slot->gainQ15 = GainQ15ForOrdinal(kAudioDiagGainProfile, slot->ordinal);
    if (slot->ordinal > 1)
    {
        OH_LOG_WARN(LOG_APP,
                    "[AudioB3] extra endpoint monoUs=%{public}llu stream=%{public}u ordinal=%{public}u gainQ15=%{public}u",
                    static_cast<unsigned long long>(MonotonicUs()),
                    streamId, slot->ordinal, slot->gainQ15);
    }

    result = OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
    if (result != AUDIOSTREAM_SUCCESS || !builder)
    {
        telemetryRendererCreateFailures_.fetch_add(1, std::memory_order_relaxed);
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] builder create failed stream=%{public}u result=%{public}d",
                     streamId, static_cast<int>(result));
        return nullptr;
    }

    result = OH_AudioStreamBuilder_SetSamplingRate(builder, kAudioMixSampleRate);
    if (result == AUDIOSTREAM_SUCCESS) result = OH_AudioStreamBuilder_SetChannelCount(builder, kAudioMixChannels);
    if (result == AUDIOSTREAM_SUCCESS) result = OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetFrameSizeInCallback(builder, kAudioTargetCallbackFrames);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, OnWriteData, slot.get());
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetRendererInterruptCallback(builder, OnInterrupt, slot.get());
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetRendererOutputDeviceChangeCallback(builder, OnOutputDeviceChange, slot.get());
    if (result == AUDIOSTREAM_SUCCESS)
        (void)OH_AudioStreamBuilder_SetRendererErrorCallback(builder, OnRendererError, slot.get());

    if (result != AUDIOSTREAM_SUCCESS)
    {
        telemetryRendererCreateFailures_.fetch_add(1, std::memory_order_relaxed);
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] builder configure failed stream=%{public}u result=%{public}d",
                     streamId, static_cast<int>(result));
        OH_AudioStreamBuilder_Destroy(builder);
        return nullptr;
    }

    result = OH_AudioStreamBuilder_GenerateRenderer(builder, &slot->renderer);
    OH_AudioStreamBuilder_Destroy(builder);
    if (result != AUDIOSTREAM_SUCCESS || !slot->renderer)
    {
        slot->renderer = nullptr;
        telemetryRendererCreateFailures_.fetch_add(1, std::memory_order_relaxed);
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] renderer generate failed stream=%{public}u result=%{public}d",
                     streamId, static_cast<int>(result));
        return nullptr;
    }

    if (OH_AudioRenderer_GetFrameSizeInCallback(slot->renderer, &callbackFrames) == AUDIOSTREAM_SUCCESS &&
        callbackFrames > 0)
        slot->callbackFrames = static_cast<uint32_t>(callbackFrames);
    else
        slot->callbackFrames = kAudioTargetCallbackFrames;

    if (kAudioDiagP2Capture)
    {
        if (!slot->ringCapture.Allocate(kPcmCaptureCapacityFrames) ||
            !slot->outCapture.Allocate(kPcmCaptureCapacityFrames))
        {
            OH_LOG_WARN(LOG_APP,
                        "[AudioB3] p2 alloc failed stream=%{public}u", streamId);
        }
    }

    OH_LOG_INFO(LOG_APP,
                "[AudioBroker] renderer created stream=%{public}u rate=48000 ch=2 usage=GAME callbackFrames=%{public}u",
                streamId, slot->callbackFrames);

    AudioRendererSlot* raw = slot.get();
    renderers_.emplace(streamId, std::move(slot));
    return raw;
}

void AudioBroker::ReleaseRendererLocked(uint32_t streamId)
{
    auto it = renderers_.find(streamId);
    if (it == renderers_.end()) return;

    std::unique_ptr<AudioRendererSlot> slot = std::move(it->second);
    renderers_.erase(it);
    if (!slot) return;

    slot->wantStarted.store(false, std::memory_order_relaxed);
    slot->fadeDir.store(0, std::memory_order_relaxed);
    if (slot->renderer)
    {
        StopRendererPlaybackLocked(slot.get());
        OH_AudioRenderer_Release(slot->renderer);
        slot->renderer = nullptr;
    }
}

int32_t AudioBroker::StartRendererLocked(AudioRendererSlot* slot)
{
    OH_AudioStream_Result result;

    if (!slot || !slot->renderer) return -EIO;
    if (slot->rendererStarted) return 0;

    // Arm fade-in before Start(). OH_AudioRenderer_Start can invoke the
    // write callback before this function returns; if gain is 0 and remaining
    // is still 0, that callback will scale the whole buffer by 0 and stick.
    BeginFadeLocked(slot, 1);
    const uint64_t tStart = MonotonicNs();
    result = OH_AudioRenderer_Start(slot->renderer);
    const uint64_t startWallNs = MonotonicNs() - tStart;
    if (result != AUDIOSTREAM_SUCCESS)
    {
        slot->fadeDir.store(0, std::memory_order_relaxed);
        slot->fadeRemaining.store(0, std::memory_order_relaxed);
        slot->gain.store(1.0f, std::memory_order_relaxed);
        telemetryRendererStartFailures_.fetch_add(1, std::memory_order_relaxed);
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] renderer start failed stream=%{public}u result=%{public}d",
                     slot->streamId, static_cast<int>(result));
        return -EIO;
    }

    slot->rendererStarted = true;
    physicalStartCount_.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(physicalStartWallMaxNs_, startWallNs);
    OH_LOG_INFO(LOG_APP,
                "[AudioB3] physicalStart monoUs=%{public}llu stream=%{public}u ordinal=%{public}u wallUs=%{public}llu gen=%{public}llu",
                static_cast<unsigned long long>(MonotonicUs()),
                slot->streamId,
                slot->ordinal,
                static_cast<unsigned long long>(startWallNs / 1000ull),
                static_cast<unsigned long long>(slot->pcmGeneration.load(std::memory_order_relaxed)));
    return 0;
}

void AudioBroker::StopRendererPlaybackLocked(AudioRendererSlot* slot)
{
    if (!slot || !slot->renderer || !slot->rendererStarted) return;
    const uint64_t tStop = MonotonicNs();
    OH_AudioRenderer_Stop(slot->renderer);
    const uint64_t stopWallNs = MonotonicNs() - tStop;
    slot->rendererStarted = false;
    slot->fadeDir.store(0, std::memory_order_relaxed);
    slot->fadeRemaining.store(0, std::memory_order_relaxed);
    slot->gain.store(1.0f, std::memory_order_relaxed);
    physicalStopCount_.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(physicalStopWallMaxNs_, stopWallNs);
    OH_LOG_INFO(LOG_APP,
                "[AudioB3] physicalStop monoUs=%{public}llu stream=%{public}u ordinal=%{public}u wallUs=%{public}llu",
                static_cast<unsigned long long>(MonotonicUs()),
                slot->streamId,
                slot->ordinal,
                static_cast<unsigned long long>(stopWallNs / 1000ull));
}

void AudioBroker::BeginFadeLocked(AudioRendererSlot* slot, int32_t dir)
{
    if (!slot) return;
    if (dir > 0)
    {
        slot->gain.store(0.0f, std::memory_order_relaxed);
        slot->fadeDir.store(1, std::memory_order_relaxed);
        slot->fadeRemaining.store(kAudioRampFrames, std::memory_order_relaxed);
        slot->fadeDeadlineNs = 0;
    }
    else if (dir < 0)
    {
        slot->fadeDir.store(-1, std::memory_order_relaxed);
        slot->fadeRemaining.store(kAudioRampFrames, std::memory_order_relaxed);
        slot->fadeDeadlineNs = MonotonicNs() + kAudioFadeTimeoutNs;
    }
}

void AudioBroker::PumpRendererLifecycleLocked(AudioRendererSlot* slot)
{
    if (!slot || !slot->renderer || !slot->stream) return;

    const bool want = slot->wantStarted.load(std::memory_order_relaxed);
    const uint32_t queued = slot->stream->queued_frames();
    const uint32_t primeNeed = slot->callbackFrames * kAudioPrimePeriods;
    const uint64_t now = MonotonicNs();

    if (want && !slot->rendererStarted)
    {
        if (queued >= primeNeed || now >= slot->primeDeadlineNs)
        {
            const int32_t startStatus = StartRendererLocked(slot);
            OH_LOG_INFO(LOG_APP,
                        "[AudioBroker] renderer prime stream=%{public}u queued=%{public}u need=%{public}u started=%{public}d",
                        slot->streamId, queued, primeNeed, startStatus == 0 ? 1 : 0);
        }
        return;
    }

    if (!want && slot->rendererStarted)
    {
        const uint32_t remaining = slot->fadeRemaining.load(std::memory_order_relaxed);
        const int32_t dir = slot->fadeDir.load(std::memory_order_relaxed);
        if (dir >= 0)
            BeginFadeLocked(slot, -1);
        if ((dir < 0 && remaining == 0) || (slot->fadeDeadlineNs && now >= slot->fadeDeadlineNs))
        {
            StopRendererPlaybackLocked(slot);
            OH_LOG_INFO(LOG_APP, "[AudioBroker] renderer stopped after fade stream=%{public}u", slot->streamId);
        }
    }
}

bool AudioBroker::EnsureCapturerLocked()
{
    OH_AudioStreamBuilder* builder = nullptr;
    OH_AudioStream_Result result = AUDIOSTREAM_SUCCESS;
    int32_t callbackFrames = 0;

    if (capturer_) return true;

    result = OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_CAPTURER);
    if (result != AUDIOSTREAM_SUCCESS || !builder)
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] capturer builder create failed result=%{public}d",
                     static_cast<int>(result));
        return false;
    }

    result = OH_AudioStreamBuilder_SetSamplingRate(builder, kAudioMixSampleRate);
    if (result == AUDIOSTREAM_SUCCESS) result = OH_AudioStreamBuilder_SetChannelCount(builder, kAudioMixChannels);
    if (result == AUDIOSTREAM_SUCCESS) result = OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetCapturerInfo(builder, AUDIOSTREAM_SOURCE_TYPE_MIC);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetFrameSizeInCallback(builder, kAudioTargetCallbackFrames);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetCapturerReadDataCallback(builder, OnReadData, this);

    if (result != AUDIOSTREAM_SUCCESS)
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] capturer builder configure failed result=%{public}d",
                     static_cast<int>(result));
        OH_AudioStreamBuilder_Destroy(builder);
        return false;
    }

    result = OH_AudioStreamBuilder_GenerateCapturer(builder, &capturer_);
    OH_AudioStreamBuilder_Destroy(builder);
    if (result != AUDIOSTREAM_SUCCESS || !capturer_)
    {
        capturer_ = nullptr;
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] capturer generate failed result=%{public}d",
                     static_cast<int>(result));
        return false;
    }

    if (OH_AudioCapturer_GetFrameSizeInCallback(capturer_, &callbackFrames) == AUDIOSTREAM_SUCCESS &&
        callbackFrames > 0)
        capturerCallbackFrames_ = static_cast<uint32_t>(callbackFrames);
    else
        capturerCallbackFrames_ = kAudioTargetCallbackFrames;

    capturerRunning_ = false;
    OH_LOG_INFO(LOG_APP, "[AudioBroker] capturer ready rate=48000 ch=2 callbackFrames=%{public}u",
                capturerCallbackFrames_);
    return true;
}

void AudioBroker::StopCapturerLocked()
{
    if (!capturer_) return;
    if (capturerRunning_) OH_AudioCapturer_Stop(capturer_);
    OH_AudioCapturer_Release(capturer_);
    capturer_ = nullptr;
    capturerRunning_ = false;
    capturerCallbackFrames_ = 0;
}

bool AudioBroker::HasStartedCaptureStreamLocked() const
{
    for (const auto& [streamId, stream] : streams_)
    {
        if (stream && stream->direction() == AudioStreamDirection::Capture && stream->started())
            return true;
    }
    return false;
}

void AudioBroker::PublishSnapshotsLocked()
{
    auto captureSnapshot = std::make_shared<StreamSnapshot>();

    captureSnapshot->reserve(streams_.size());
    for (const auto& [streamId, stream] : streams_)
    {
        if (!stream || stream->state() == WINEHUA_AUDIO_STREAM_CLOSED) continue;
        if (stream->direction() == AudioStreamDirection::Capture)
            captureSnapshot->push_back(stream);
    }

    std::shared_ptr<const StreamSnapshot> constCaptureSnapshot = captureSnapshot;
    std::atomic_store_explicit(&captureSnapshot_, constCaptureSnapshot, std::memory_order_release);
}

bool AudioBroker::FillRendererCallback(AudioRendererSlot* slot,
                                       int16_t* dst,
                                       uint32_t frames,
                                       uint64_t* ringReadNs,
                                       uint64_t* metricsNs)
{
    size_t sampleCount = static_cast<size_t>(frames) * kAudioMixChannels;
    size_t gotFrames;
    const bool wineStarted = slot && slot->stream && slot->stream->started();
    const int32_t fadeDir = slot ? slot->fadeDir.load(std::memory_order_relaxed) : 0;
    const uint32_t fadeRemainingLoaded = slot ? slot->fadeRemaining.load(std::memory_order_relaxed) : 0;
    uint32_t fadeRemaining = fadeRemainingLoaded;
    const bool fadingOut = fadeDir < 0;
    float gain;

    if (ringReadNs) *ringReadNs = 0;
    if (metricsNs) *metricsNs = 0;
    if (!slot || !slot->stream || !dst || !frames) return false;
    if (slot->stream->direction() != AudioStreamDirection::Render) return false;
    if (!wineStarted && !fadingOut) return false;

    slot->requestedFrames.fetch_add(frames, std::memory_order_relaxed);
    NoteMinQueued(&slot->minQueuedFrames, slot->stream->queued_frames());

    const uint64_t tRead = MonotonicNs();
    gotFrames = slot->stream->ReadFrames(dst, frames);
    if (ringReadNs) *ringReadNs = MonotonicNs() - tRead;
    slot->consumedFrames.fetch_add(static_cast<uint64_t>(gotFrames), std::memory_order_relaxed);

    const uint64_t tMetrics = MonotonicNs();
    const uint64_t generation = slot->pcmGeneration.load(std::memory_order_relaxed);
    if (gotFrames)
    {
        slot->ringMetrics.Add(AnalyzeStereoS16(dst, static_cast<uint32_t>(gotFrames),
                                                slot->ringContinuity, generation));
        if (kAudioDiagP2Capture)
            slot->ringCapture.CaptureFromCallback(dst, static_cast<uint32_t>(gotFrames), tMetrics);
    }

    if (gotFrames < frames)
    {
        slot->stream->MarkMixUnderrun();
        slot->underrunEvents.fetch_add(1, std::memory_order_relaxed);
        slot->underrunFrames.fetch_add(frames - gotFrames, std::memory_order_relaxed);
        if (!gotFrames)
        {
            if (!fadingOut && (!slot->stream->has_hold() || slot->stream->consecutive_underruns() > 2))
            {
                if (metricsNs) *metricsNs = MonotonicNs() - tMetrics;
                return false;
            }
            if (slot->stream->has_hold())
                slot->stream->HoldPadRemainder(dst, 0, frames);
            else
                std::memset(dst, 0, sampleCount * sizeof(int16_t));
        }
        else
        {
            slot->stream->NoteLastFrame(dst, gotFrames);
            slot->stream->HoldPadRemainder(dst, gotFrames, frames);
        }
    }
    else
    {
        slot->stream->MarkMixFilled();
        slot->stream->NoteLastFrame(dst, gotFrames);
    }

    gain = slot->gain.load(std::memory_order_relaxed);
    if (wineStarted && !fadingOut && fadeRemaining == 0 && gain == 0.0f)
    {
        gain = 1.0f;
        slot->gain.store(1.0f, std::memory_order_relaxed);
        slot->fadeRecoverCount.fetch_add(1, std::memory_order_relaxed);
    }
    else if (fadeRemaining || gain != 1.0f)
    {
        const float target = fadeDir < 0 ? 0.0f : 1.0f;
        ApplyLinearRampS16(dst, frames, &gain, target, &fadeRemaining);
        slot->gain.store(gain, std::memory_order_relaxed);
        const uint32_t remainingNow = slot->fadeRemaining.load(std::memory_order_relaxed);
        if (remainingNow <= fadeRemainingLoaded)
            slot->fadeRemaining.store(fadeRemaining, std::memory_order_relaxed);
        if (!fadeRemaining && fadeDir != 0)
            slot->broker->RequestLifecyclePump();
    }

    if (slot->gainQ15 != kUnityGainQ15)
        ApplyStereoGainQ15(dst, frames, slot->gainQ15);

    const PcmBlockMetrics out = AnalyzeStereoS16(dst, frames, slot->outputContinuity, generation);
    slot->outMetrics.Add(out);
    if (kAudioDiagP2Capture)
        slot->outCapture.CaptureFromCallback(dst, frames, MonotonicNs());
    slot->outMetrics.validCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (out.hasFrames)
    {
        slot->lastValidL.store(out.lastL, std::memory_order_relaxed);
        slot->lastValidR.store(out.lastR, std::memory_order_relaxed);
        slot->haveLastValid.store(true, std::memory_order_relaxed);
        if (slot->expectFirstAfterStart.load(std::memory_order_relaxed))
        {
            uint32_t startL = out.boundaryDeltaL;
            uint32_t startR = out.boundaryDeltaR;
            if (slot->haveLastValidBeforeStop.load(std::memory_order_relaxed))
            {
                startL = AbsI32(out.firstL - slot->lastValidBeforeStopL.load(std::memory_order_relaxed));
                startR = AbsI32(out.firstR - slot->lastValidBeforeStopR.load(std::memory_order_relaxed));
            }
            AtomicMax(slot->startBoundaryMaxL, startL);
            AtomicMax(slot->startBoundaryMaxR, startR);
            slot->expectFirstAfterStart.store(false, std::memory_order_relaxed);
        }
        if (slot->sawStopEdge.load(std::memory_order_relaxed))
        {
            AtomicMax(slot->stopBoundaryMaxL, out.boundaryDeltaL);
            AtomicMax(slot->stopBoundaryMaxR, out.boundaryDeltaR);
            slot->sawStopEdge.store(false, std::memory_order_relaxed);
        }
    }

    const int peak = static_cast<int>(out.peakL > out.peakR ? out.peakL : out.peakR);
    NotePeak(&slot->peakAbs, peak);
    NotePeak(&slot->maxAdjacentDelta,
             static_cast<int>(out.maxDeltaL > out.maxDeltaR ? out.maxDeltaL : out.maxDeltaR));
    if (metricsNs) *metricsNs = MonotonicNs() - tMetrics;
    return true;
}

void AudioBroker::DistributeCaptureFramesS16(const std::shared_ptr<const StreamSnapshot>& snapshot,
                                             const int16_t* src, uint32_t frames)
{
    if (!snapshot || snapshot->empty() || !src || !frames) return;

    for (const auto& stream : *snapshot)
    {
        size_t written;

        if (!stream || stream->direction() != AudioStreamDirection::Capture || !stream->started()) continue;

        written = stream->WriteFrames(src, frames);
        if (written < frames) stream->IncrementOverflow();
    }
}

OH_AudioData_Callback_Result AudioBroker::OnWriteData(OH_AudioRenderer* renderer,
                                                      void* userData,
                                                      void* audioData,
                                                      int32_t audioDataSize)
{
    auto* slot = static_cast<AudioRendererSlot*>(userData);
    uint32_t frames;

    (void)renderer;
    if (!slot || !slot->broker || !audioData || audioDataSize <= 0)
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;

    frames = static_cast<uint32_t>(audioDataSize / kAudioMixFrameSize);
    if (!frames || frames > kAudioMaxCallbackFrames)
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;

    const uint64_t t0 = MonotonicNs();
    uint64_t ringReadNs = 0;
    uint64_t metricsNs = 0;
    slot->callbacks.fetch_add(1, std::memory_order_relaxed);
    slot->broker->NoteCallbackTiming(slot, frames);
    const bool ok = slot->broker->FillRendererCallback(slot, static_cast<int16_t*>(audioData),
                                                     frames, &ringReadNs, &metricsNs);
    const uint64_t wallNs = MonotonicNs() - t0;
    slot->callbackWallTotalNs.fetch_add(wallNs, std::memory_order_relaxed);
    AtomicMax(slot->callbackWallMaxNs, wallNs);
    slot->ringReadTotalNs.fetch_add(ringReadNs, std::memory_order_relaxed);
    slot->metricsTotalNs.fetch_add(metricsNs, std::memory_order_relaxed);
    if (!ok)
    {
        slot->outMetrics.invalidCallbacks.fetch_add(1, std::memory_order_relaxed);
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
    }
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

void AudioBroker::NoteCallbackTiming(AudioRendererSlot* slot, uint32_t frames)
{
    if (!slot) return;

    const uint64_t now = MonotonicNs();
    const uint64_t prev = slot->lastCallbackNs.exchange(now, std::memory_order_relaxed);
    if (!prev || !frames) return;

    const uint64_t dt = now - prev;
    uint64_t maxNs = slot->intervalMaxNs.load(std::memory_order_relaxed);
    while (dt > maxNs &&
           !slot->intervalMaxNs.compare_exchange_weak(maxNs, dt, std::memory_order_relaxed))
    {
    }

    const uint64_t nominal = (static_cast<uint64_t>(frames) * 1000000000ull) / kAudioMixSampleRate;
    if (dt > nominal + nominal / 2)
        slot->lateCallbacks.fetch_add(1, std::memory_order_relaxed);
}

void AudioBroker::MaybeArmP2Locked()
{
    uint32_t started = 0;
    for (const auto& [id, slot] : renderers_)
    {
        (void)id;
        if (slot && slot->stream && slot->stream->started())
            ++started;
    }

    if (kAudioDiagP2Capture &&
        p2CapturesStarted_ < kAudioDiagP2MaxCaptures &&
        p2LastStartedCount_ == 1 && started >= 2)
    {
        ++p2CapturesStarted_;
        const uint64_t captureId = p2CapturesStarted_;
        for (auto& [id, slot] : renderers_)
        {
            (void)id;
            if (!slot)
                continue;
            slot->ringCapture.Arm(captureId);
            slot->outCapture.Arm(captureId);
        }
        OH_LOG_INFO(LOG_APP,
                    "[AudioB3] p2arm captureId=%{public}llu started=%{public}u",
                    static_cast<unsigned long long>(captureId), started);
    }
    p2LastStartedCount_ = started;
}

void AudioBroker::FlushP2Captures()
{
    struct Job
    {
        uint32_t streamId = 0;
        uint32_t ordinal = 0;
        uint64_t captureId = 0;
        uint64_t firstNs = 0;
        uint32_t ringFrames = 0;
        uint32_t ringDropped = 0;
        uint32_t outFrames = 0;
        uint32_t outDropped = 0;
        std::vector<int16_t> ring;
        std::vector<int16_t> out;
    };

    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, slot] : renderers_)
        {
            if (!slot)
                continue;
            PcmDiagnosticCapture::ReadyCapture ring{};
            PcmDiagnosticCapture::ReadyCapture out{};
            const bool hasRing = slot->ringCapture.PeekReady(&ring);
            const bool hasOut = slot->outCapture.PeekReady(&out);
            if (!hasRing && !hasOut)
                continue;

            Job job;
            job.streamId = slot->streamId;
            job.ordinal = slot->ordinal;
            if (hasRing && ring.samples)
            {
                job.captureId = ring.captureId;
                job.firstNs = ring.firstTimestampNs;
                job.ringFrames = ring.frames;
                job.ringDropped = ring.droppedFrames;
                job.ring.assign(ring.samples,
                              ring.samples + static_cast<size_t>(ring.frames) * 2);
                slot->ringCapture.ResetToIdle();
            }
            if (hasOut && out.samples)
            {
                if (!job.captureId)
                    job.captureId = out.captureId;
                if (!job.firstNs)
                    job.firstNs = out.firstTimestampNs;
                job.outFrames = out.frames;
                job.outDropped = out.droppedFrames;
                job.out.assign(out.samples,
                               out.samples + static_cast<size_t>(out.frames) * 2);
                slot->outCapture.ResetToIdle();
            }
            jobs.push_back(std::move(job));
        }
    }

    if (jobs.empty())
        return;

    char logDir[512] = {};
    WineHuaLogDirectory(logDir, sizeof(logDir));
    const char* dir = logDir[0] ? logDir : runtimeDir_.c_str();
    for (const Job& job : jobs)
    {
        char ringPath[768];
        char outPath[768];
        snprintf(ringPath, sizeof(ringPath),
                 "%s/p2_c%llu_e%u_s%u_ring.s16le",
                 dir,
                 static_cast<unsigned long long>(job.captureId),
                 job.ordinal,
                 job.streamId);
        snprintf(outPath, sizeof(outPath),
                 "%s/p2_c%llu_e%u_s%u_out.s16le",
                 dir,
                 static_cast<unsigned long long>(job.captureId),
                 job.ordinal,
                 job.streamId);
        if (job.ringFrames)
        {
            FILE* fp = fopen(ringPath, "wb");
            if (fp)
            {
                fwrite(job.ring.data(), sizeof(int16_t), job.ring.size(), fp);
                fclose(fp);
            }
        }
        if (job.outFrames)
        {
            FILE* fp = fopen(outPath, "wb");
            if (fp)
            {
                fwrite(job.out.data(), sizeof(int16_t), job.out.size(), fp);
                fclose(fp);
            }
        }
        OH_LOG_INFO(LOG_APP,
                    "[AudioB3] p2write captureId=%{public}llu stream=%{public}u ordinal=%{public}u firstNs=%{public}llu ringFrames=%{public}u ringDrop=%{public}u outFrames=%{public}u outDrop=%{public}u path=%{public}s",
                    static_cast<unsigned long long>(job.captureId),
                    job.streamId,
                    job.ordinal,
                    static_cast<unsigned long long>(job.firstNs),
                    job.ringFrames,
                    job.ringDropped,
                    job.outFrames,
                    job.outDropped,
                    ringPath);
    }
}

void AudioBroker::LifecycleLoop()
{
    while (!workerStop_.load(std::memory_order_relaxed))
    {
        std::unique_lock<std::mutex> lock(mutex_);
        lifecycleCv_.wait_for(lock, std::chrono::milliseconds(5),
                               [this] { return workerStop_.load(std::memory_order_relaxed); });
        if (workerStop_.load(std::memory_order_relaxed)) break;
        for (auto& [streamId, slot] : renderers_)
            PumpRendererLifecycleLocked(slot.get());
    }
}

void AudioBroker::TelemetryLoop()
{
    while (!workerStop_.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < 10 && !workerStop_.load(std::memory_order_relaxed); ++i)
            usleep(100000);

        if (workerStop_.load(std::memory_order_relaxed)) break;

        FlushP2Captures();

        struct SlotSnap
        {
            uint32_t streamId = 0;
            uint32_t ordinal = 0;
            uint32_t gainQ15 = kUnityGainQ15;
            uint64_t generation = 0;
            bool want = false;
            bool physical = false;
            bool wineStarted = false;
            int32_t fadeDir = 0;
            uint32_t fadeRemaining = 0;
            int32_t fadeGainX1000 = 1000;
            uint32_t fadeRecover = 0;
            uint64_t callbacks = 0;
            uint64_t underrunEv = 0;
            uint64_t underrunFr = 0;
            uint64_t requested = 0;
            uint64_t consumed = 0;
            uint64_t late = 0;
            uint32_t minQueued = ~0u;
            int32_t peak = 0;
            int32_t maxDelta = 0;
            uint64_t intervalMaxNs = 0;
            uint64_t wallTotalNs = 0;
            uint64_t wallMaxNs = 0;
            uint64_t ringReadTotalNs = 0;
            uint64_t metricsTotalNs = 0;
            // Sample the platform sink on this worker, never on its real-time
            // callback. A full SHM ring does not rule out a downstream xrun.
            uint32_t sinkUnderflows = 0;
            int32_t sinkUnderflowResult = -1;
            int32_t sinkTimestampResult = -1;
            int64_t sinkFramePosition = 0;
            int64_t sinkTimestampNs = 0;
            uint32_t startBoundL = 0;
            uint32_t startBoundR = 0;
            uint32_t stopBoundL = 0;
            uint32_t stopBoundR = 0;
            PcmMetricSnapshot ring;
            PcmMetricSnapshot out;
        };

        uint32_t rendererCount = 0;
        uint32_t streams = 0;
        uint64_t callbacks = 0;
        uint64_t underrunEv = 0;
        uint64_t underrunFr = 0;
        uint64_t requested = 0;
        uint64_t consumed = 0;
        uint32_t minQueued = ~0u;
        int32_t peak = 0;
        int32_t maxDelta = 0;
        uint64_t intervalMaxNs = 0;
        uint64_t late = 0;
        uint32_t createFail = telemetryRendererCreateFailures_.exchange(0, std::memory_order_relaxed);
        uint32_t startFail = telemetryRendererStartFailures_.exchange(0, std::memory_order_relaxed);
        uint32_t interrupts = telemetryInterruptCount_.exchange(0, std::memory_order_relaxed);
        uint32_t resumes = telemetryResumeCount_.exchange(0, std::memory_order_relaxed);
        uint32_t routeGen = telemetryRouteGeneration_.load(std::memory_order_relaxed);
        const uint64_t getStatusCalls = getStatusCalls_.exchange(0, std::memory_order_relaxed);
        const uint64_t getStatusTotalNs = getStatusLatencyTotalNs_.exchange(0, std::memory_order_relaxed);
        const uint64_t getStatusMaxNs = getStatusLatencyMaxNs_.exchange(0, std::memory_order_relaxed);
        const uint32_t physicalStarts = physicalStartCount_.exchange(0, std::memory_order_relaxed);
        const uint32_t physicalStops = physicalStopCount_.exchange(0, std::memory_order_relaxed);
        const uint64_t physicalStartMaxNs = physicalStartWallMaxNs_.exchange(0, std::memory_order_relaxed);
        const uint64_t physicalStopMaxNs = physicalStopWallMaxNs_.exchange(0, std::memory_order_relaxed);
        std::vector<SlotSnap> snaps;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            rendererCount = static_cast<uint32_t>(renderers_.size());
            snaps.reserve(renderers_.size());
            for (const auto& [streamId, slot] : renderers_)
            {
                if (!slot) continue;
                SlotSnap snap;
                snap.streamId = slot->streamId;
                snap.ordinal = slot->ordinal;
                snap.gainQ15 = slot->gainQ15;
                snap.generation = slot->pcmGeneration.load(std::memory_order_relaxed);
                snap.want = slot->wantStarted.load(std::memory_order_relaxed);
                snap.physical = slot->rendererStarted;
                if (slot->renderer && snap.physical)
                {
                    snap.sinkUnderflowResult = OH_AudioRenderer_GetUnderflowCount(
                        slot->renderer, &snap.sinkUnderflows);
                    snap.sinkTimestampResult = OH_AudioRenderer_GetTimestamp(
                        slot->renderer, CLOCK_MONOTONIC, &snap.sinkFramePosition, &snap.sinkTimestampNs);
                }
                snap.wineStarted = slot->stream && slot->stream->started();
                snap.fadeDir = slot->fadeDir.load(std::memory_order_relaxed);
                snap.fadeRemaining = slot->fadeRemaining.load(std::memory_order_relaxed);
                snap.fadeGainX1000 = static_cast<int32_t>(
                    slot->gain.load(std::memory_order_relaxed) * 1000.0f);
                snap.fadeRecover = slot->fadeRecoverCount.exchange(0, std::memory_order_relaxed);
                if (snap.want) streams++;
                snap.callbacks = slot->callbacks.exchange(0, std::memory_order_relaxed);
                snap.underrunEv = slot->underrunEvents.exchange(0, std::memory_order_relaxed);
                snap.underrunFr = slot->underrunFrames.exchange(0, std::memory_order_relaxed);
                snap.requested = slot->requestedFrames.exchange(0, std::memory_order_relaxed);
                snap.consumed = slot->consumedFrames.exchange(0, std::memory_order_relaxed);
                snap.late = slot->lateCallbacks.exchange(0, std::memory_order_relaxed);
                snap.minQueued = slot->minQueuedFrames.exchange(~0u, std::memory_order_relaxed);
                snap.peak = slot->peakAbs.exchange(0, std::memory_order_relaxed);
                snap.maxDelta = slot->maxAdjacentDelta.exchange(0, std::memory_order_relaxed);
                snap.intervalMaxNs = slot->intervalMaxNs.exchange(0, std::memory_order_relaxed);
                snap.wallTotalNs = slot->callbackWallTotalNs.exchange(0, std::memory_order_relaxed);
                snap.wallMaxNs = slot->callbackWallMaxNs.exchange(0, std::memory_order_relaxed);
                snap.ringReadTotalNs = slot->ringReadTotalNs.exchange(0, std::memory_order_relaxed);
                snap.metricsTotalNs = slot->metricsTotalNs.exchange(0, std::memory_order_relaxed);
                snap.startBoundL = slot->startBoundaryMaxL.exchange(0, std::memory_order_relaxed);
                snap.startBoundR = slot->startBoundaryMaxR.exchange(0, std::memory_order_relaxed);
                snap.stopBoundL = slot->stopBoundaryMaxL.exchange(0, std::memory_order_relaxed);
                snap.stopBoundR = slot->stopBoundaryMaxR.exchange(0, std::memory_order_relaxed);
                snap.ring = slot->ringMetrics.ExchangeReset();
                snap.out = slot->outMetrics.ExchangeReset();
                callbacks += snap.callbacks;
                underrunEv += snap.underrunEv;
                underrunFr += snap.underrunFr;
                requested += snap.requested;
                consumed += snap.consumed;
                late += snap.late;
                if (snap.minQueued < minQueued) minQueued = snap.minQueued;
                if (snap.peak > peak) peak = snap.peak;
                if (snap.maxDelta > maxDelta) maxDelta = snap.maxDelta;
                if (snap.intervalMaxNs > intervalMaxNs) intervalMaxNs = snap.intervalMaxNs;
                snaps.push_back(snap);
            }
        }

        if (!callbacks && !underrunEv && !createFail && !startFail && !interrupts && !resumes &&
            !getStatusCalls && !physicalStarts && !physicalStops)
            continue;

        const uint64_t monoUs = MonotonicUs();
        OH_LOG_INFO(LOG_APP,
                    "[AudioBroker] telemetry renderers=%{public}u streams=%{public}u callbacks=%{public}llu underrunEv=%{public}llu underrunFr=%{public}llu requested=%{public}llu consumed=%{public}llu minQueued=%{public}u peak=%{public}d maxDelta=%{public}d intervalMaxUs=%{public}llu late=%{public}llu createFail=%{public}u startFail=%{public}u interrupts=%{public}u resumes=%{public}u routeGen=%{public}u",
                    rendererCount,
                    streams,
                    static_cast<unsigned long long>(callbacks),
                    static_cast<unsigned long long>(underrunEv),
                    static_cast<unsigned long long>(underrunFr),
                    static_cast<unsigned long long>(requested),
                    static_cast<unsigned long long>(consumed),
                    minQueued,
                    static_cast<int>(peak),
                    static_cast<int>(maxDelta),
                    static_cast<unsigned long long>(intervalMaxNs / 1000ull),
                    static_cast<unsigned long long>(late),
                    createFail,
                    startFail,
                    interrupts,
                    resumes,
                    routeGen);

        for (const SlotSnap& snap : snaps)
        {
            if (!snap.callbacks && !snap.out.validCallbacks && !snap.out.invalidCallbacks &&
                !snap.ring.frames && !snap.fadeRecover)
                continue;
            const uint64_t avgWallUs = snap.callbacks
                ? (snap.wallTotalNs / snap.callbacks) / 1000ull
                : 0;
            OH_LOG_INFO(LOG_APP,
                        "[AudioB3] pcm monoUs=%{public}llu commit=%{public}s profile=%{public}s stream=%{public}u ordinal=%{public}u gen=%{public}llu logical=%{public}s physical=%{public}s gainQ15=%{public}u fadeGainX1000=%{public}d fadeRem=%{public}u fadeRecover=%{public}u ringFrames=%{public}llu ringPeakL=%{public}u ringPeakR=%{public}u ringFullL=%{public}u ringFullR=%{public}u ringRunL=%{public}u ringRunR=%{public}u ringNearFullL=%{public}u ringNearFullR=%{public}u outPeakL=%{public}u outPeakR=%{public}u outFullL=%{public}u outFullR=%{public}u outRunL=%{public}u outRunR=%{public}u deltaL=%{public}u deltaR=%{public}u boundaryL=%{public}u boundaryR=%{public}u rmsL=%{public}u rmsR=%{public}u dcL=%{public}d dcR=%{public}d validCb=%{public}u invalidCb=%{public}u startBoundL=%{public}u startBoundR=%{public}u stopBoundL=%{public}u stopBoundR=%{public}u click16L=%{public}u click16R=%{public}u click32L=%{public}u click32R=%{public}u",
                        static_cast<unsigned long long>(monoUs),
                        kAudioDiagGitCommit,
                        AudioDiagGainProfileName(kAudioDiagGainProfile),
                        snap.streamId,
                        snap.ordinal,
                        static_cast<unsigned long long>(snap.generation),
                        LogicalRenderName(snap.want, snap.physical, snap.wineStarted, snap.fadeDir),
                        PhysicalRenderName(snap.physical),
                        snap.gainQ15,
                        static_cast<int>(snap.fadeGainX1000),
                        snap.fadeRemaining,
                        snap.fadeRecover,
                        static_cast<unsigned long long>(snap.ring.frames),
                        snap.ring.peakL,
                        snap.ring.peakR,
                        snap.ring.fullScaleSamplesL,
                        snap.ring.fullScaleSamplesR,
                        snap.ring.maxFullScaleRunL,
                        snap.ring.maxFullScaleRunR,
                        snap.ring.nearFullScaleSamplesL,
                        snap.ring.nearFullScaleSamplesR,
                        snap.out.peakL,
                        snap.out.peakR,
                        snap.out.fullScaleSamplesL,
                        snap.out.fullScaleSamplesR,
                        snap.out.maxFullScaleRunL,
                        snap.out.maxFullScaleRunR,
                        snap.out.maxDeltaL,
                        snap.out.maxDeltaR,
                        snap.out.maxBoundaryDeltaL,
                        snap.out.maxBoundaryDeltaR,
                        RmsFromSumSquares(snap.out.sumSquaresL, snap.out.frames),
                        RmsFromSumSquares(snap.out.sumSquaresR, snap.out.frames),
                        DcFromSum(snap.out.sumL, snap.out.frames),
                        DcFromSum(snap.out.sumR, snap.out.frames),
                        snap.out.validCallbacks,
                        snap.out.invalidCallbacks,
                        snap.startBoundL,
                        snap.startBoundR,
                        snap.stopBoundL,
                        snap.stopBoundR,
                        snap.ring.click16L,
                        snap.ring.click16R,
                        snap.ring.click32L,
                        snap.ring.click32R);
            OH_LOG_INFO(LOG_APP,
                        "[AudioB3] cbperf monoUs=%{public}llu stream=%{public}u ordinal=%{public}u callbacks=%{public}llu wallAvgUs=%{public}llu wallMaxUs=%{public}llu intervalMaxUs=%{public}llu ringReadUs=%{public}llu metricsUs=%{public}llu late=%{public}llu underrunEv=%{public}llu requested=%{public}llu consumed=%{public}llu",
                        static_cast<unsigned long long>(monoUs),
                        snap.streamId,
                        snap.ordinal,
                        static_cast<unsigned long long>(snap.callbacks),
                        static_cast<unsigned long long>(avgWallUs),
                        static_cast<unsigned long long>(snap.wallMaxNs / 1000ull),
                        static_cast<unsigned long long>(snap.intervalMaxNs / 1000ull),
                        static_cast<unsigned long long>(snap.ringReadTotalNs / 1000ull),
                        static_cast<unsigned long long>(snap.metricsTotalNs / 1000ull),
                        static_cast<unsigned long long>(snap.late),
                        static_cast<unsigned long long>(snap.underrunEv),
                        static_cast<unsigned long long>(snap.requested),
                        static_cast<unsigned long long>(snap.consumed));
            OH_LOG_INFO(LOG_APP,
                        "[AudioB3] sink monoUs=%{public}llu stream=%{public}u ordinal=%{public}u underflowResult=%{public}d underflows=%{public}u timestampResult=%{public}d position=%{public}lld timestampNs=%{public}lld",
                        static_cast<unsigned long long>(monoUs), snap.streamId, snap.ordinal,
                        snap.sinkUnderflowResult, snap.sinkUnderflows, snap.sinkTimestampResult,
                        static_cast<long long>(snap.sinkFramePosition),
                        static_cast<long long>(snap.sinkTimestampNs));
        }

        const uint64_t getStatusAvgUs = getStatusCalls
            ? (getStatusTotalNs / getStatusCalls) / 1000ull
            : 0;
        OH_LOG_INFO(LOG_APP,
                    "[AudioB3] ipc monoUs=%{public}llu commit=%{public}s profile=%{public}s getStatusCalls=%{public}llu getStatusAvgUs=%{public}llu getStatusMaxUs=%{public}llu physicalStart=%{public}u physicalStop=%{public}u startWallMaxUs=%{public}llu stopWallMaxUs=%{public}llu activeStreams=%{public}u renderers=%{public}u",
                    static_cast<unsigned long long>(monoUs),
                    kAudioDiagGitCommit,
                    AudioDiagGainProfileName(kAudioDiagGainProfile),
                    static_cast<unsigned long long>(getStatusCalls),
                    static_cast<unsigned long long>(getStatusAvgUs),
                    static_cast<unsigned long long>(getStatusMaxNs / 1000ull),
                    physicalStarts,
                    physicalStops,
                    static_cast<unsigned long long>(physicalStartMaxNs / 1000ull),
                    static_cast<unsigned long long>(physicalStopMaxNs / 1000ull),
                    streams,
                    rendererCount);
    }
}

void AudioBroker::OnReadData(OH_AudioCapturer* capturer,
                             void* userData,
                             void* audioData,
                             int32_t audioDataSize)
{
    auto* broker = static_cast<AudioBroker*>(userData);
    auto snapshot = broker ? std::atomic_load_explicit(&broker->captureSnapshot_, std::memory_order_acquire) : nullptr;
    uint32_t frames;

    (void)capturer;
    if (!broker || !audioData || audioDataSize <= 0) return;

    frames = static_cast<uint32_t>(audioDataSize / kAudioMixFrameSize);
    if (!frames) return;

    broker->DistributeCaptureFramesS16(snapshot, static_cast<const int16_t*>(audioData), frames);
}

void AudioBroker::OnInterrupt(OH_AudioRenderer* renderer,
                              void* userData,
                              OH_AudioInterrupt_ForceType type,
                              OH_AudioInterrupt_Hint hint)
{
    auto* slot = static_cast<AudioRendererSlot*>(userData);

    (void)renderer;
    if (!slot || !slot->broker) return;

    OH_LOG_INFO(LOG_APP,
                "[AudioBroker] renderer interrupt stream=%{public}u hint=%{public}d force=%{public}d",
                slot->streamId, static_cast<int>(hint), static_cast<int>(type));
    OH_LOG_INFO(LOG_APP,
                "[AudioB3] interrupt monoUs=%{public}llu stream=%{public}u ordinal=%{public}u hint=%{public}d force=%{public}d physical=%{public}s",
                static_cast<unsigned long long>(MonotonicUs()),
                slot->streamId, slot->ordinal, static_cast<int>(hint), static_cast<int>(type),
                PhysicalRenderName(slot->rendererStarted));

    std::lock_guard<std::mutex> lock(slot->broker->mutex_);
    slot->broker->telemetryInterruptCount_.fetch_add(1, std::memory_order_relaxed);

    if (hint == AUDIOSTREAM_INTERRUPT_HINT_PAUSE || hint == AUDIOSTREAM_INTERRUPT_HINT_STOP)
    {
        slot->rendererStarted = false;
        return;
    }

    if (hint != AUDIOSTREAM_INTERRUPT_HINT_RESUME) return;

    slot->broker->telemetryResumeCount_.fetch_add(1, std::memory_order_relaxed);
    if (!slot->wantStarted.load(std::memory_order_relaxed)) return;

    slot->rendererStarted = false;
    slot->primeDeadlineNs = MonotonicNs() + kAudioPrimeTimeoutNs;
    slot->broker->PumpRendererLifecycleLocked(slot);
    slot->broker->RequestLifecyclePump();
    OH_LOG_INFO(LOG_APP, "[AudioBroker] renderer resume requested stream=%{public}u", slot->streamId);
}

void AudioBroker::OnOutputDeviceChange(OH_AudioRenderer* renderer,
                                        void* userData,
                                        OH_AudioStream_DeviceChangeReason reason)
{
    auto* slot = static_cast<AudioRendererSlot*>(userData);

    (void)renderer;
    if (!slot || !slot->broker) return;

    const uint32_t generation = slot->broker->telemetryRouteGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    OH_LOG_INFO(LOG_APP,
                "[AudioBroker] renderer route change stream=%{public}u reason=%{public}d generation=%{public}u",
                slot->streamId, static_cast<int>(reason), generation);
    OH_LOG_INFO(LOG_APP,
                "[AudioB3] route monoUs=%{public}llu stream=%{public}u ordinal=%{public}u reason=%{public}d routeGen=%{public}u",
                static_cast<unsigned long long>(MonotonicUs()),
                slot->streamId, slot->ordinal, static_cast<int>(reason), generation);

    std::lock_guard<std::mutex> lock(slot->broker->mutex_);
    slot->broker->BumpPcmGeneration(slot);
    if (!slot->wantStarted.load(std::memory_order_relaxed)) return;
    if (slot->rendererStarted) return;
    slot->primeDeadlineNs = MonotonicNs() + kAudioPrimeTimeoutNs;
    slot->broker->PumpRendererLifecycleLocked(slot);
    slot->broker->RequestLifecyclePump();
}

void AudioBroker::OnRendererError(OH_AudioRenderer* renderer,
                                 void* userData,
                                 OH_AudioStream_Result error)
{
    auto* slot = static_cast<AudioRendererSlot*>(userData);

    (void)renderer;
    if (!slot || !slot->broker) return;

    OH_LOG_ERROR(LOG_APP, "[AudioBroker] renderer error stream=%{public}u result=%{public}d",
                 slot->streamId, static_cast<int>(error));
    OH_LOG_ERROR(LOG_APP,
                 "[AudioB3] error monoUs=%{public}llu stream=%{public}u ordinal=%{public}u result=%{public}d",
                 static_cast<unsigned long long>(MonotonicUs()),
                 slot->streamId, slot->ordinal, static_cast<int>(error));

    std::lock_guard<std::mutex> lock(slot->broker->mutex_);
    slot->broker->BumpPcmGeneration(slot);
    slot->rendererStarted = false;
    if (!slot->wantStarted.load(std::memory_order_relaxed)) return;
    slot->primeDeadlineNs = MonotonicNs() + kAudioPrimeTimeoutNs;
    slot->broker->PumpRendererLifecycleLocked(slot);
    slot->broker->RequestLifecyclePump();
}

} // namespace winehua
