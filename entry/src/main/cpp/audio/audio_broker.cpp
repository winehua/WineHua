#include "audio_broker.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_AUDIO"
#include <hilog/log.h>

#include "audio_ipc_server.h"
#include "common/ring_buffer.h"

namespace winehua {

namespace {

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

} // namespace

AudioBroker& AudioBroker::GetInstance()
{
    static AudioBroker broker;
    return broker;
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

    if (!EnsureRendererLocked()) return false;

    server_ = std::make_unique<AudioIpcServer>();
    if (!server_->Start(this))
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start bootstrap server");
        StopRendererLocked();
        server_.reset();
        return false;
    }

    running_ = true;
    OH_LOG_INFO(LOG_APP, "[AudioBroker] started runtimeDir=%{public}s callbackFrames=%{public}u",
                runtimeDir_.c_str(), rendererCallbackFrames_);
    return true;
}

void AudioBroker::Stop()
{
    std::unique_ptr<AudioIpcServer> server;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) return;

        running_ = false;
        server = std::move(server_);
    }

    if (server) server->Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [streamId, stream] : streams_)
    {
        LogStreamStats("broker-stop", stream);
        stream->MarkClosed();
    }
    streams_.clear();
    PublishSnapshotsLocked();
    StopCapturerLocked();
    StopRendererLocked();
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
    else if (!renderer_)
    {
        return -EIO;
    }

    format.sampleRate = kAudioMixSampleRate;
    format.channels = kAudioMixChannels;
    format.sampleFormat = WINEHUA_AUDIO_SAMPLE_S16LE;
    format.frameSize = kAudioMixFrameSize;
    format.preferredPeriodFrames = isCapture
        ? (capturerCallbackFrames_ ? capturerCallbackFrames_ : kAudioTargetCallbackFrames)
        : (rendererCallbackFrames_ ? rendererCallbackFrames_ : kAudioTargetCallbackFrames);

    streamId = nextStreamId_++;
    stream = std::make_shared<AudioStream>(
        streamId, connectionId, clientPid, processName, format,
        ComputeRingCapacityFrames(format.preferredPeriodFrames),
        isCapture ? AudioStreamDirection::Capture : AudioStreamDirection::Render);
    if (!stream->Create())
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create ring for stream=%{public}u", streamId);
        return -EIO;
    }

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
                "[AudioBroker] open stream id=%{public}u conn=%{public}u pid=%{public}u name=%{public}s ringFrames=%{public}u period=%{public}u",
                streamId, connectionId, clientPid, processName.c_str(),
                resp->ring_capacity_frames, resp->preferred_period_frames);

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
    it->second->SetStarted(false);

    if (it->second->direction() == AudioStreamDirection::Capture && capturerRunning_ && capturer_ &&
        !HasStartedCaptureStreamLocked())
    {
        OH_AudioCapturer_Stop(capturer_);
        capturerRunning_ = false;
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

bool AudioBroker::EnsureRendererLocked()
{
    OH_AudioStreamBuilder* builder = nullptr;
    OH_AudioStream_Result result = AUDIOSTREAM_SUCCESS;
    int32_t callbackFrames = 0;
    size_t maxSamples = 0;

    if (renderer_) return true;

    result = OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
    if (result != AUDIOSTREAM_SUCCESS || !builder)
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] builder create failed result=%{public}d", static_cast<int>(result));
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
        result = OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetFrameSizeInCallback(builder, kAudioTargetCallbackFrames);
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, OnWriteData, this);
    // 注册打断回调：来电/通话等高优先级音频会打断 GAME 流，挂断后系统发送
    // RESUME，必须在此回调里显式重新 Start 渲染流，否则音频永久无声。
    if (result == AUDIOSTREAM_SUCCESS)
        result = OH_AudioStreamBuilder_SetRendererInterruptCallback(builder, OnInterrupt, this);

    if (result != AUDIOSTREAM_SUCCESS)
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] builder configure failed result=%{public}d", static_cast<int>(result));
        OH_AudioStreamBuilder_Destroy(builder);
        return false;
    }

    result = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer_);
    OH_AudioStreamBuilder_Destroy(builder);
    if (result != AUDIOSTREAM_SUCCESS || !renderer_)
    {
        renderer_ = nullptr;
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] renderer generate failed result=%{public}d", static_cast<int>(result));
        return false;
    }

    if (OH_AudioRenderer_GetFrameSizeInCallback(renderer_, &callbackFrames) == AUDIOSTREAM_SUCCESS &&
        callbackFrames > 0)
        rendererCallbackFrames_ = static_cast<uint32_t>(callbackFrames);
    else
        rendererCallbackFrames_ = kAudioTargetCallbackFrames;

    maxSamples = static_cast<size_t>(kAudioMaxCallbackFrames) * kAudioMixChannels;
    mixScratch_.assign(maxSamples, 0);
    mixAccum_.assign(maxSamples, 0);

    result = OH_AudioRenderer_Start(renderer_);
    if (result != AUDIOSTREAM_SUCCESS)
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] renderer start failed result=%{public}d", static_cast<int>(result));
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
        return false;
    }

    OH_LOG_INFO(LOG_APP, "[AudioBroker] renderer ready rate=48000 ch=2 callbackFrames=%{public}u",
                rendererCallbackFrames_);
    return true;
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

void AudioBroker::StopRendererLocked()
{
    if (!renderer_) return;
    OH_AudioRenderer_Stop(renderer_);
    OH_AudioRenderer_Release(renderer_);
    renderer_ = nullptr;
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
    auto renderSnapshot = std::make_shared<StreamSnapshot>();
    auto captureSnapshot = std::make_shared<StreamSnapshot>();

    renderSnapshot->reserve(streams_.size());
    captureSnapshot->reserve(streams_.size());
    for (const auto& [streamId, stream] : streams_)
    {
        if (!stream || stream->state() == WINEHUA_AUDIO_STREAM_CLOSED) continue;

        if (stream->direction() == AudioStreamDirection::Capture)
            captureSnapshot->push_back(stream);
        else
            renderSnapshot->push_back(stream);
    }

    std::shared_ptr<const StreamSnapshot> constRenderSnapshot = renderSnapshot;
    std::shared_ptr<const StreamSnapshot> constCaptureSnapshot = captureSnapshot;
    std::atomic_store_explicit(&renderSnapshot_, constRenderSnapshot, std::memory_order_release);
    std::atomic_store_explicit(&captureSnapshot_, constCaptureSnapshot, std::memory_order_release);
}

void AudioBroker::MixStreamsS16(const std::shared_ptr<const StreamSnapshot>& snapshot, int16_t* dst, uint32_t frames)
{
    size_t sampleCount = static_cast<size_t>(frames) * kAudioMixChannels;
    bool any = false;

    std::memset(dst, 0, sampleCount * sizeof(int16_t));
    if (!snapshot || snapshot->empty() || sampleCount > mixScratch_.size() || sampleCount > mixAccum_.size()) return;

    std::memset(mixAccum_.data(), 0, sampleCount * sizeof(int32_t));
    for (const auto& stream : *snapshot)
    {
        size_t gotFrames;
        size_t gotSamples;

        if (!stream || stream->direction() != AudioStreamDirection::Render || !stream->started()) continue;

        gotFrames = stream->ReadFrames(mixScratch_.data(), frames);
        if (gotFrames < frames) stream->IncrementUnderrun();
        if (!gotFrames) continue;

        any = true;
        gotSamples = gotFrames * kAudioMixChannels;
        for (size_t i = 0; i < gotSamples; ++i)
            mixAccum_[i] += mixScratch_[i];
    }

    if (!any) return;

    for (size_t i = 0; i < sampleCount; ++i)
    {
        int mixed = mixAccum_[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        dst[i] = static_cast<int16_t>(mixed);
    }
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
    auto* broker = static_cast<AudioBroker*>(userData);
    uint32_t frames;
    auto snapshot = broker ? std::atomic_load_explicit(&broker->renderSnapshot_, std::memory_order_acquire) : nullptr;

    (void)renderer;
    if (!broker || !audioData || audioDataSize <= 0) return AUDIO_DATA_CALLBACK_RESULT_INVALID;

    frames = static_cast<uint32_t>(audioDataSize / kAudioMixFrameSize);
    if (!frames || frames > kAudioMaxCallbackFrames)
    {
        std::memset(audioData, 0, static_cast<size_t>(audioDataSize));
        return AUDIO_DATA_CALLBACK_RESULT_VALID;
    }

    broker->MixStreamsS16(snapshot, static_cast<int16_t*>(audioData), frames);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
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
    auto* broker = static_cast<AudioBroker*>(userData);

    (void)renderer;
    OH_LOG_INFO(LOG_APP, "[AudioBroker] renderer interrupt hint=%{public}d force=%{public}d",
                static_cast<int>(hint), static_cast<int>(type));
    if (!broker) return;

    // 来电/通话等高优先级音频以 FORCE 方式打断 GAME 渲染流，系统会把流暂停；
    // 挂断后系统发送 RESUME，若此时不显式重新 Start，渲染流将停在暂停态，
    // Wine 内所有音频永久无声。RESUME 时重新启动即可恢复发声。
    if (hint == AUDIOSTREAM_INTERRUPT_HINT_RESUME)
    {
        std::lock_guard<std::mutex> lock(broker->mutex_);
        if (broker->renderer_)
        {
            OH_AudioRenderer_Start(broker->renderer_);
            OH_LOG_INFO(LOG_APP, "[AudioBroker] renderer resumed after interrupt");
        }
    }
    // PAUSE/STOP/DUCK/UNDUCK/MUTE/UNMUTE：系统已代为处理（暂停/停止/音量），
    // 应用侧无需额外动作；恢复统一由 RESUME 驱动。
}

} // namespace winehua
