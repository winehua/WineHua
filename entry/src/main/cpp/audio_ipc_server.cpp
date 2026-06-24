#include "audio_ipc_server.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WL_AUDIO"
#include <hilog/log.h>

#include "audio_broker.h"
#include "audio_ipc_protocol.h"

namespace winehua {

namespace {

bool SetCloseOnExec(int fd, bool enabled)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    if (enabled) flags |= FD_CLOEXEC;
    else flags &= ~FD_CLOEXEC;
    return fcntl(fd, F_SETFD, flags) == 0;
}

ssize_t SendPacket(int fd, const void* data, size_t bytes, int sendFd)
{
    struct msghdr msg{};
    struct iovec iov{};
    char control[CMSG_SPACE(sizeof(int))];

    iov.iov_base = const_cast<void*>(data);
    iov.iov_len = bytes;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (sendFd >= 0)
    {
        struct cmsghdr* cmsg;

        std::memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &sendFd, sizeof(sendFd));
        msg.msg_controllen = cmsg->cmsg_len;
    }

    while (true)
    {
        ssize_t sent = sendmsg(fd, &msg, 0);
        if (sent >= 0) return sent;
        if (errno == EINTR) continue;
        return -1;
    }
}

ssize_t RecvPacket(int fd, void* data, size_t bytes, int* receivedFd)
{
    struct msghdr msg{};
    struct iovec iov{};
    char control[CMSG_SPACE(sizeof(int))];
    ssize_t got;

    if (receivedFd) *receivedFd = -1;

    iov.iov_base = data;
    iov.iov_len = bytes;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    while (true)
    {
        got = recvmsg(fd, &msg, 0);
        if (got >= 0) break;
        if (errno == EINTR) continue;
        return -1;
    }

    if (got <= 0) return got;
    if (msg.msg_flags & MSG_TRUNC) return -1;
    if (receivedFd)
    {
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int)))
            {
                std::memcpy(receivedFd, CMSG_DATA(cmsg), sizeof(int));
                break;
            }
        }
    }

    return got;
}

template <typename T>
bool SendResponse(int fd, uint16_t cmd, uint32_t seq, const T& payload, int sendFd = -1)
{
    struct
    {
        WinehuaAudioIpcHeader header;
        T payload;
    } packet{};

    packet.header.magic = WINEHUA_AUDIO_CONTROL_MAGIC;
    packet.header.version = WINEHUA_AUDIO_PROTOCOL_VERSION;
    packet.header.cmd = cmd;
    packet.header.seq = seq;
    packet.header.payload_size = sizeof(T);
    packet.payload = payload;
    return SendPacket(fd, &packet, sizeof(packet), sendFd) == static_cast<ssize_t>(sizeof(packet));
}

} // namespace

AudioIpcServer::AudioIpcServer() = default;

AudioIpcServer::~AudioIpcServer()
{
    Stop();
}

bool AudioIpcServer::Start(AudioBroker* broker)
{
    if (running_) return true;
    broker_ = broker;
    running_ = true;
    return true;
}

void AudioIpcServer::Stop()
{
    std::vector<std::unique_ptr<BootstrapEndpoint>> endpoints;
    std::vector<std::unique_ptr<ClientThreadSlot>> clientThreads;

    if (!running_) return;
    running_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints.swap(bootstrapEndpoints_);
        clientThreads = std::move(clientThreads_);
        for (int clientFd : clientFds_)
            shutdown(clientFd, SHUT_RDWR);
    }

    for (auto& endpoint : endpoints)
    {
        endpoint->running = false;
        if (endpoint->hostFd >= 0)
        {
            shutdown(endpoint->hostFd, SHUT_RDWR);
            close(endpoint->hostFd);
            endpoint->hostFd = -1;
        }
    }

    for (auto& endpoint : endpoints)
    {
        if (endpoint->thread.joinable()) endpoint->thread.join();
    }

    for (auto& thread : clientThreads)
    {
        if (thread->thread.joinable()) thread->thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        clientFds_.clear();
    }
}

int AudioIpcServer::CreateBootstrapHandle()
{
    int socketPair[2]{-1, -1};
    auto endpoint = std::make_unique<BootstrapEndpoint>();
    BootstrapEndpoint* endpointPtr = endpoint.get();
    std::vector<std::unique_ptr<BootstrapEndpoint>> finishedEndpoints;
    std::vector<std::unique_ptr<ClientThreadSlot>> finishedClientThreads;

    if (!running_) return -1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = bootstrapEndpoints_.begin(); it != bootstrapEndpoints_.end();)
        {
            if (!(*it)->running && (*it)->hostFd < 0)
            {
                finishedEndpoints.push_back(std::move(*it));
                it = bootstrapEndpoints_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = clientThreads_.begin(); it != clientThreads_.end();)
        {
            if (!(*it)->running)
            {
                finishedClientThreads.push_back(std::move(*it));
                it = clientThreads_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& finished : finishedEndpoints)
    {
        if (finished->thread.joinable()) finished->thread.join();
    }
    for (auto& finished : finishedClientThreads)
    {
        if (finished->thread.joinable()) finished->thread.join();
    }

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, socketPair) != 0)
    {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] socketpair failed errno=%{public}d", errno);
        return -1;
    }

    SetCloseOnExec(socketPair[0], true);
    SetCloseOnExec(socketPair[1], false);

    endpoint->hostFd = socketPair[0];
    endpoint->running = true;
    endpoint->thread = std::thread(&AudioIpcServer::BootstrapLoop, this, endpointPtr);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bootstrapEndpoints_.push_back(std::move(endpoint));
    }

    return socketPair[1];
}

void AudioIpcServer::BootstrapLoop(BootstrapEndpoint* endpoint)
{
    while (running_ && endpoint->running)
    {
        uint8_t packet[WINEHUA_AUDIO_PACKET_MAX];
        WinehuaAudioBootstrapReq req{};
        int clientFd = -1;
        uint32_t connectionId;
        ssize_t got = RecvPacket(endpoint->hostFd, packet, sizeof(packet), &clientFd);

        if (got == 0) break;
        if (got < 0)
        {
            if (!running_) break;
            OH_LOG_WARN(LOG_APP, "[AudioBroker] bootstrap recv failed errno=%{public}d", errno);
            continue;
        }

        if (clientFd < 0 || static_cast<size_t>(got) != sizeof(req))
        {
            if (clientFd >= 0) close(clientFd);
            OH_LOG_WARN(LOG_APP, "[AudioBroker] invalid bootstrap packet size=%{public}zd fd=%{public}d", got, clientFd);
            continue;
        }

        std::memcpy(&req, packet, sizeof(req));
        if (req.magic != WINEHUA_AUDIO_BOOTSTRAP_MAGIC ||
            req.version != WINEHUA_AUDIO_PROTOCOL_VERSION ||
            req.cmd != WINEHUA_AUDIO_BOOTSTRAP_OPEN_CONTROL)
        {
            close(clientFd);
            OH_LOG_WARN(LOG_APP, "[AudioBroker] invalid bootstrap packet magic=%{public}u cmd=%{public}u",
                        req.magic, req.cmd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            connectionId = nextConnectionId_++;
            clientFds_.insert(clientFd);
        }

        OH_LOG_INFO(LOG_APP, "[AudioBroker] bootstrap accepted conn=%{public}u pid=%{public}u name=%{public}s",
                    connectionId, req.client_pid, req.process_name);
        try
        {
            auto clientSlot = std::make_unique<ClientThreadSlot>();
            ClientThreadSlot* slotPtr = clientSlot.get();
            std::vector<std::unique_ptr<ClientThreadSlot>> finishedClientThreads;

            clientSlot->running = true;
            clientSlot->thread = std::thread(&AudioIpcServer::HandleClient, this, slotPtr, clientFd, connectionId,
                                             req.client_pid,
                                             std::string(req.process_name, strnlen(req.process_name, sizeof(req.process_name))));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                clientThreads_.push_back(std::move(clientSlot));
                for (auto it = clientThreads_.begin(); it != clientThreads_.end();)
                {
                    if (!(*it)->running)
                    {
                        finishedClientThreads.push_back(std::move(*it));
                        it = clientThreads_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            for (auto& finished : finishedClientThreads)
            {
                if (finished->thread.joinable()) finished->thread.join();
            }
        }
        catch (const std::system_error& e)
        {
            OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start client thread conn=%{public}u err=%{public}s",
                         connectionId, e.what());
            UnregisterClientFd(clientFd);
            close(clientFd);
        }
    }

    endpoint->running = false;
    if (endpoint->hostFd >= 0)
    {
        close(endpoint->hostFd);
        endpoint->hostFd = -1;
    }
}

void AudioIpcServer::HandleClient(ClientThreadSlot* slot,
                                  int clientFd,
                                  uint32_t connectionId,
                                  uint32_t bootstrapPid,
                                  std::string processName)
{
    while (running_)
    {
        uint8_t packet[WINEHUA_AUDIO_PACKET_MAX];
        WinehuaAudioIpcHeader header{};
        uint8_t* payload = packet + sizeof(header);
        size_t payloadSize;
        int receivedFd = -1;
        ssize_t got = RecvPacket(clientFd, packet, sizeof(packet), &receivedFd);

        if (got == 0) break;
        if (got < 0)
        {
            if (!running_) break;
            OH_LOG_WARN(LOG_APP, "[AudioBroker] client recv failed conn=%{public}u errno=%{public}d", connectionId, errno);
            break;
        }
        if (receivedFd >= 0) close(receivedFd);
        if (static_cast<size_t>(got) < sizeof(header)) break;

        std::memcpy(&header, packet, sizeof(header));
        payloadSize = static_cast<size_t>(got) - sizeof(header);
        if (header.magic != WINEHUA_AUDIO_CONTROL_MAGIC ||
            header.version != WINEHUA_AUDIO_PROTOCOL_VERSION ||
            payloadSize != header.payload_size)
            break;

        switch (header.cmd)
        {
        case WINEHUA_AUDIO_CMD_HELLO:
        {
            WinehuaAudioHelloReq req{};
            WinehuaAudioHelloResp resp{};

            if (payloadSize != sizeof(req)) goto done;
            std::memcpy(&req, payload, sizeof(req));
            if (req.process_name[0]) processName.assign(req.process_name, strnlen(req.process_name, sizeof(req.process_name)));
            if (!req.client_pid) req.client_pid = bootstrapPid;
            resp.result = broker_ ? broker_->RegisterClient(connectionId, req, &resp) : -EIO;
            if (!SendResponse(clientFd, header.cmd, header.seq, resp)) goto done;
            break;
        }
        case WINEHUA_AUDIO_CMD_OPEN_STREAM:
        {
            WinehuaAudioOpenStreamReq req{};
            WinehuaAudioOpenStreamResp resp{};
            int ringFd = -1;

            if (payloadSize != sizeof(req)) goto done;
            std::memcpy(&req, payload, sizeof(req));
            resp.result = broker_ ? broker_->OpenStream(connectionId, bootstrapPid, processName, req, &resp, &ringFd) : -EIO;
            if (!SendResponse(clientFd, header.cmd, header.seq, resp, ringFd)) goto done;
            break;
        }
        case WINEHUA_AUDIO_CMD_START:
        case WINEHUA_AUDIO_CMD_STOP:
        case WINEHUA_AUDIO_CMD_RESET:
        case WINEHUA_AUDIO_CMD_CLOSE:
        {
            WinehuaAudioStreamCmdReq req{};
            WinehuaAudioStreamCmdResp resp{};

            if (payloadSize != sizeof(req)) goto done;
            std::memcpy(&req, payload, sizeof(req));
            resp.stream_id = req.stream_id;
            if (!broker_) resp.result = -EIO;
            else if (header.cmd == WINEHUA_AUDIO_CMD_START) resp.result = broker_->StartStream(connectionId, req.stream_id);
            else if (header.cmd == WINEHUA_AUDIO_CMD_STOP) resp.result = broker_->StopStream(connectionId, req.stream_id);
            else if (header.cmd == WINEHUA_AUDIO_CMD_RESET) resp.result = broker_->ResetStream(connectionId, req.stream_id);
            else resp.result = broker_->CloseStream(connectionId, req.stream_id);
            if (!SendResponse(clientFd, header.cmd, header.seq, resp)) goto done;
            break;
        }
        case WINEHUA_AUDIO_CMD_GET_STATUS:
        {
            WinehuaAudioGetStatusReq req{};
            WinehuaAudioGetStatusResp resp{};

            if (payloadSize != sizeof(req)) goto done;
            std::memcpy(&req, payload, sizeof(req));
            resp.result = broker_ ? broker_->GetStatus(connectionId, req.stream_id, &resp) : -EIO;
            if (!SendResponse(clientFd, header.cmd, header.seq, resp)) goto done;
            break;
        }
        default:
            goto done;
        }
    }

done:
    if (broker_) broker_->CleanupClientStreams(connectionId);
    UnregisterClientFd(clientFd);
    close(clientFd);
    if (slot) slot->running = false;
}

void AudioIpcServer::UnregisterClientFd(int clientFd)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clientFds_.erase(clientFd);
}

} // namespace winehua
