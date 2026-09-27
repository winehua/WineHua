// CreateNativeChildProcess bootstrap for a future opt-in Direct Wine path.
// The existing StartNativeChildProcess Main(NativeChildProcess_Args) entry is unchanged.
#include "wine_child_ipc.h"

#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <hilog/log.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define LOG_DOMAIN 0x2330
#define LOG_TAG "WineChildIPC"

extern "C" void Main(NativeChildProcess_Args args);

namespace {

struct NamedFd {
    std::string name;
    int fd = -1;
};

std::mutex g_mutex;
std::condition_variable g_ready;
bool g_received = false;
std::string g_params;
std::vector<NamedFd> g_fds;

void CloseFds(std::vector<NamedFd>& fds)
{
    for (auto& item : fds) {
        if (item.fd >= 0) close(item.fd);
        item.fd = -1;
    }
}

int OnRequest(uint32_t code, const OHIPCParcel* request, OHIPCParcel* reply, void*)
{
    if (code != winehua::wineipc::kBootstrap || !request || !reply)
        return OH_IPC_CHECK_PARAM_ERROR;

    int32_t version = 0;
    int32_t count = -1;
    if (OH_IPCParcel_ReadInt32(request, &version) != OH_IPC_SUCCESS ||
        version != winehua::wineipc::kVersion)
        return OH_IPC_CHECK_PARAM_ERROR;
    const char* params = OH_IPCParcel_ReadString(request);
    if (!params || std::strlen(params) > 16384 ||
        OH_IPCParcel_ReadInt32(request, &count) != OH_IPC_SUCCESS ||
        count < 0 || count > winehua::wineipc::kMaxFds)
        return OH_IPC_CHECK_PARAM_ERROR;

    std::vector<NamedFd> received;
    received.reserve(count);
    for (int32_t i = 0; i < count; ++i) {
        const char* name = OH_IPCParcel_ReadString(request);
        int32_t fd = -1;
        if (!name || !name[0] || std::strlen(name) > 20 ||
            OH_IPCParcel_ReadFileDescriptor(request, &fd) != OH_IPC_SUCCESS || fd < 0) {
            CloseFds(received);
            return OH_IPC_CHECK_PARAM_ERROR;
        }
        received.push_back({name, fd});
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_received) {
            CloseFds(received);
            return OH_IPC_CHECK_PARAM_ERROR;
        }
        g_params = params;
        g_fds = std::move(received);
        g_received = true;
    }
    g_ready.notify_one();
    return OH_IPCParcel_WriteInt32(reply, getpid());
}

void RunProbe(std::vector<NamedFd>& fds)
{
    int input = -1;
    int output = -1;
    for (const auto& item : fds) {
        if (item.name == "probe_input") input = item.fd;
        if (item.name == "probe_output") output = item.fd;
    }
    winehua::wineipc::ProbeResult result{getpid(), -1, 0, static_cast<int32_t>(fds.size())};
    if (input >= 0 && output >= 0 && fds.size() == 2 &&
        read(input, &result.token, sizeof(result.token)) == sizeof(result.token) &&
        result.token == winehua::wineipc::kProbeToken)
        result.status = 0;
    if (output >= 0) (void)write(output, &result, sizeof(result));
    CloseFds(fds);
}

} // namespace

extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect()
{
    return OH_IPCRemoteStub_Create("winehua.direct.wine.bootstrap", OnRequest, nullptr, nullptr);
}

extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc()
{
    std::string params;
    std::vector<NamedFd> fds;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_ready.wait_for(lock, std::chrono::seconds(15), [] { return g_received; })) {
            OH_LOG_ERROR(LOG_APP, "[WineChildIPC] bootstrap timeout pid=%{public}d", getpid());
            return;
        }
        params = std::move(g_params);
        fds = std::move(g_fds);
    }
    if (params == winehua::wineipc::kProbeParams) {
        RunProbe(fds);
        return;
    }

    // Own storage for the complete Main call. fdName pointers refer to fds strings.
    std::vector<NativeChildProcess_Fd> nodes(fds.size());
    for (size_t i = 0; i < fds.size(); ++i) {
        nodes[i].fdName = const_cast<char*>(fds[i].name.c_str());
        nodes[i].fd = fds[i].fd;
        nodes[i].next = i + 1 < fds.size() ? &nodes[i + 1] : nullptr;
    }
    NativeChildProcess_Args args{};
    args.entryParams = const_cast<char*>(params.c_str());
    args.fdList.head = nodes.empty() ? nullptr : &nodes[0];
    Main(args);
    // Main may close or hand off its fds; process exit releases any survivors.
}
