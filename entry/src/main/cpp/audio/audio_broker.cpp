#include "audio_broker.h"

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

#include "audio_ipc_server.h"
#include "common/ring_buffer.h"

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

int MaxAdjacentDelta(const int16_t* samples, uint32_t frames)
{
    int peak = 0;
    if (!samples || frames < 2) return 0;
    const uint32_t sampleCount = frames * kAudioMixChannels;
    for (uint32_t i = 1; i < sampleCount; ++i)
    {
        int delta = static_cast<int>(samples[i]) - static_cast<int>(samples[i - 1]);
        if (delta < 0) delta = -delta;
        if (delta > peak) peak = delta;
    }
    return peak;
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
        slot->fadeDir.store(0, std::memory_order_relaxed);
        slot->fadeRemaining.store(0, std::memory_order_relaxed);
        PumpRendererLifecycleLocked(slot);
        RequestLifecyclePump();
        OH_LOG_INFO(LOG_APP,
                    "[AudioBroker] start stream id=%{public}u queued=%{public}u rendererStarted=%{public}d",
                    streamId, slot->stream ? slot->stream->queued_frames() : 0,
                    slot->rendererStarted ? 1 : 0);
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
            if (slot->rendererStarted)
                BeginFadeLocked(slot, -1);
            it->second->SetStarted(false);
            PumpRendererLifecycleLocked(slot);
            RequestLifecyclePump();
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
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    int32_t ownerStatus = ValidateStreamOwner(it == streams_.end() ? nullptr : it->second,
                                              connectionId, streamId);

    if (!resp) return -EINVAL;
    std::memset(resp, 0, sizeof(*resp));
    if (ownerStatus != 0) return ownerStatus;

    resp->result = 0;
    resp->stream_id = streamId;
    resp->state = it->second->state();
    resp->queued_frames = it->second->queued_frames();
    resp->free_frames = it->second->free_frames();
    resp->underrun_count = it->second->underrun_count();
    resp->overflow_count = it->second->overflow_count();
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
        if (slot->rendererStarted)
        {
            OH_AudioRenderer_Stop(slot->renderer);
            slot->rendererStarted = false;
        }
        OH_AudioRenderer_Release(slot->renderer);
        slot->renderer = nullptr;
    }
}

int32_t AudioBroker::StartRendererLocked(AudioRendererSlot* slot)
{
    OH_AudioStream_Result result;

    if (!slot || !slot->renderer) return -EIO;
    if (slot->rendererStarted) return 0;

    result = OH_AudioRenderer_Start(slot->renderer);
    if (result != AUDIOSTREAM_SUCCESS)
    {
        telemetryRendererStartFailures_.fetch_add(1, std::memory_order_relaxed);
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] renderer start failed stream=%{public}u result=%{public}d",
                     slot->streamId, static_cast<int>(result));
        return -EIO;
    }

    slot->rendererStarted = true;
    BeginFadeLocked(slot, 1);
    return 0;
}

void AudioBroker::StopRendererPlaybackLocked(AudioRendererSlot* slot)
{
    if (!slot || !slot->renderer || !slot->rendererStarted) return;
    OH_AudioRenderer_Stop(slot->renderer);
    slot->rendererStarted = false;
    slot->fadeDir.store(0, std::memory_order_relaxed);
    slot->fadeRemaining.store(0, std::memory_order_relaxed);
    slot->gain.store(1.0f, std::memory_order_relaxed);
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

bool AudioBroker::FillRendererCallback(AudioRendererSlot* slot, int16_t* dst, uint32_t frames)
{
    size_t sampleCount = static_cast<size_t>(frames) * kAudioMixChannels;
    size_t gotFrames;
    int peak = 0;
    const bool wineStarted = slot && slot->stream && slot->stream->started();
    const int32_t fadeDir = slot ? slot->fadeDir.load(std::memory_order_relaxed) : 0;
    uint32_t fadeRemaining = slot ? slot->fadeRemaining.load(std::memory_order_relaxed) : 0;
    const bool fadingOut = fadeDir < 0;
    float gain;

    if (!slot || !slot->stream || !dst || !frames) return false;
    if (slot->stream->direction() != AudioStreamDirection::Render) return false;
    if (!wineStarted && !fadingOut) return false;

    slot->requestedFrames.fetch_add(frames, std::memory_order_relaxed);
    NoteMinQueued(&slot->minQueuedFrames, slot->stream->queued_frames());

    gotFrames = slot->stream->ReadFrames(dst, frames);
    slot->consumedFrames.fetch_add(static_cast<uint64_t>(gotFrames), std::memory_order_relaxed);
    if (gotFrames < frames)
    {
        slot->stream->MarkMixUnderrun();
        slot->underrunEvents.fetch_add(1, std::memory_order_relaxed);
        slot->underrunFrames.fetch_add(frames - gotFrames, std::memory_order_relaxed);
        if (!gotFrames)
        {
            if (!fadingOut && (!slot->stream->has_hold() || slot->stream->consecutive_underruns() > 2))
                return false;
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
    if (fadeRemaining || gain != 1.0f)
    {
        const float target = fadeDir < 0 ? 0.0f : 1.0f;
        ApplyLinearRampS16(dst, frames, &gain, target, &fadeRemaining);
        slot->gain.store(gain, std::memory_order_relaxed);
        slot->fadeRemaining.store(fadeRemaining, std::memory_order_relaxed);
        if (!fadeRemaining && fadeDir != 0)
            slot->broker->RequestLifecyclePump();
    }

    NotePeak(&slot->maxAdjacentDelta, MaxAdjacentDelta(dst, frames));
    for (size_t i = 0; i < sampleCount; ++i)
    {
        const int16_t sample = dst[i];
        const int absv = sample >= 0 ? sample : (sample == -32768 ? 32767 : -sample);
        if (absv > peak) peak = absv;
    }
    NotePeak(&slot->peakAbs, peak);
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

    slot->callbacks.fetch_add(1, std::memory_order_relaxed);
    slot->broker->NoteCallbackTiming(slot, frames);
    if (!slot->broker->FillRendererCallback(slot, static_cast<int16_t*>(audioData), frames))
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            rendererCount = static_cast<uint32_t>(renderers_.size());
            for (const auto& [streamId, slot] : renderers_)
            {
                if (!slot) continue;
                if (slot->wantStarted.load(std::memory_order_relaxed)) streams++;
                callbacks += slot->callbacks.exchange(0, std::memory_order_relaxed);
                underrunEv += slot->underrunEvents.exchange(0, std::memory_order_relaxed);
                underrunFr += slot->underrunFrames.exchange(0, std::memory_order_relaxed);
                requested += slot->requestedFrames.exchange(0, std::memory_order_relaxed);
                consumed += slot->consumedFrames.exchange(0, std::memory_order_relaxed);
                late += slot->lateCallbacks.exchange(0, std::memory_order_relaxed);
                const uint32_t slotMin = slot->minQueuedFrames.exchange(~0u, std::memory_order_relaxed);
                if (slotMin < minQueued) minQueued = slotMin;
                const int32_t slotPeak = slot->peakAbs.exchange(0, std::memory_order_relaxed);
                if (slotPeak > peak) peak = slotPeak;
                const int32_t slotDelta = slot->maxAdjacentDelta.exchange(0, std::memory_order_relaxed);
                if (slotDelta > maxDelta) maxDelta = slotDelta;
                const uint64_t slotInterval = slot->intervalMaxNs.exchange(0, std::memory_order_relaxed);
                if (slotInterval > intervalMaxNs) intervalMaxNs = slotInterval;
            }
        }

        if (!callbacks && !underrunEv && !createFail && !startFail && !interrupts && !resumes) continue;

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

    std::lock_guard<std::mutex> lock(slot->broker->mutex_);
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

    std::lock_guard<std::mutex> lock(slot->broker->mutex_);
    slot->rendererStarted = false;
    if (!slot->wantStarted.load(std::memory_order_relaxed)) return;
    slot->primeDeadlineNs = MonotonicNs() + kAudioPrimeTimeoutNs;
    slot->broker->PumpRendererLifecycleLocked(slot);
    slot->broker->RequestLifecyclePump();
}

} // namespace winehua
