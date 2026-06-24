#include "audio_broker.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

#undef LOG_TAG
#define LOG_TAG "WL_AUDIO"
#include <hilog/log.h>

#include "audio_ipc_server.h"
#include "ring_buffer.h"

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
    PublishSnapshotLocked();
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

    if (changed) PublishSnapshotLocked();
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

    if (!running_ || !renderer_) return -EIO;
    if (req.sample_rate != kAudioMixSampleRate || req.channels != kAudioMixChannels ||
        req.sample_format != WINEHUA_AUDIO_SAMPLE_S16LE)
        return -EINVAL;

    format.sampleRate = kAudioMixSampleRate;
    format.channels = kAudioMixChannels;
    format.sampleFormat = WINEHUA_AUDIO_SAMPLE_S16LE;
    format.frameSize = kAudioMixFrameSize;
    format.preferredPeriodFrames = rendererCallbackFrames_ ? rendererCallbackFrames_ : kAudioTargetCallbackFrames;

    streamId = nextStreamId_++;
    stream = std::make_shared<AudioStream>(
        streamId, connectionId, clientPid, processName, format, ComputeRingCapacityFrames(format.preferredPeriodFrames));
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
    PublishSnapshotLocked();
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
    it->second->MarkClosed();
    streams_.erase(it);
    PublishSnapshotLocked();
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

void AudioBroker::StopRendererLocked()
{
    if (!renderer_) return;
    OH_AudioRenderer_Stop(renderer_);
    OH_AudioRenderer_Release(renderer_);
    renderer_ = nullptr;
}

void AudioBroker::PublishSnapshotLocked()
{
    auto snapshot = std::make_shared<StreamSnapshot>();

    snapshot->reserve(streams_.size());
    for (const auto& [streamId, stream] : streams_)
    {
        if (stream && stream->state() != WINEHUA_AUDIO_STREAM_CLOSED)
            snapshot->push_back(stream);
    }

    std::shared_ptr<const StreamSnapshot> constSnapshot = snapshot;
    std::atomic_store_explicit(&renderSnapshot_, constSnapshot, std::memory_order_release);
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

        if (!stream || !stream->started()) continue;

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

} // namespace winehua
