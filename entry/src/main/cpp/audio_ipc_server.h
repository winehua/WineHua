#ifndef WINEHUA_AUDIO_IPC_SERVER_H
#define WINEHUA_AUDIO_IPC_SERVER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace winehua {

class AudioBroker;

class AudioIpcServer
{
public:
    AudioIpcServer();
    ~AudioIpcServer();

    bool Start(AudioBroker* broker);
    void Stop();
    int CreateBootstrapHandle();

private:
    struct BootstrapEndpoint
    {
        int hostFd = -1;
        std::atomic<bool> running{false};
        std::thread thread;
    };

    struct ClientThreadSlot
    {
        std::atomic<bool> running{false};
        std::thread thread;
    };

    void BootstrapLoop(BootstrapEndpoint* endpoint);
    void HandleClient(ClientThreadSlot* slot,
                      int clientFd,
                      uint32_t connectionId,
                      uint32_t bootstrapPid,
                      std::string processName);
    void UnregisterClientFd(int clientFd);

    AudioBroker* broker_ = nullptr;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    uint32_t nextConnectionId_ = 1;
    std::vector<std::unique_ptr<BootstrapEndpoint>> bootstrapEndpoints_;
    std::unordered_set<int> clientFds_;
    std::vector<std::unique_ptr<ClientThreadSlot>> clientThreads_;
};

} // namespace winehua

#endif
