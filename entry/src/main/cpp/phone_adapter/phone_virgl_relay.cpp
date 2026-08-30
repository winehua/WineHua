// phone_virgl_relay.cpp — 手机适配层：virgl IPC 中继（parent 侧）
// 手机 fork 路径下没有 Binder 通道，通过配置 socket 做请求/响应中继。
// 线协议（native 字节序）：
//   请求: [u32 code][u32 payloadLen][payload]
//   响应: [u32 payloadLen][payload]   （query=原始 struct 字节；其余=int32 result）
// payload 字段顺序与 virgl_child.cpp OnVirglIpcRequest 的 parcel 解析严格一致。
#include "phone_virgl_relay.h"
#include "phone_socket.h"
#include "phone_adapter.h"       // PhoneAdapter_GetConfigSocket
#include "graphics/virgl_ipc_protocol.h"

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <vector>

#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#include <hilog/log.h>

namespace {

std::mutex g_relayMutex;  // 单 socket 流，串行化请求/响应

} // namespace

int PhoneVirgl_RelayRequest(uint32_t code, const OHIPCParcel* data, OHIPCParcel* reply) {
    namespace vi = winehua::virgl_ipc;
    namespace sock = phone_adapter;

    int fd = PhoneAdapter_GetConfigSocket();
    if (fd < 0 || !data) return kPhoneVirglRelayError;

    std::lock_guard<std::mutex> lock(g_relayMutex);

    if (!sock::SockWritePod(fd, code)) return kPhoneVirglRelayError;

    switch (code) {
    case vi::kConfigureRequest: {
        int32_t version = 0;
        const char* s[vi::kHostConfigStringCount] = {};
        if (OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS) return kPhoneVirglRelayError;
        uint32_t len = sizeof(version);
        for (uint32_t i = 0; i < vi::kHostConfigStringCount; ++i) {
            s[i] = OH_IPCParcel_ReadString(data);
            if (!s[i]) return kPhoneVirglRelayError;
            len += sizeof(uint32_t) + static_cast<uint32_t>(strlen(s[i])) + 1;
        }
        if (!sock::SockWritePod(fd, len) || !sock::SockWritePod(fd, version)) return kPhoneVirglRelayError;
        for (uint32_t i = 0; i < vi::kHostConfigStringCount; ++i)
            if (!sock::SockWriteStr(fd, s[i])) return kPhoneVirglRelayError;
        break;
    }
    case vi::kDetachSurfaceRequest: {
        int32_t version = 0; int64_t key = 0;
        if (OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS ||
            OH_IPCParcel_ReadInt64(data, &key) != OH_IPC_SUCCESS) return kPhoneVirglRelayError;
        uint32_t len = sizeof(version) + sizeof(key);
        if (!sock::SockWritePod(fd, len) || !sock::SockWritePod(fd, version) ||
            !sock::SockWritePod(fd, key)) return kPhoneVirglRelayError;
        break;
    }
    case vi::kSetFramePeriodRequest: {
        int32_t version = 0; int64_t key = 0, period = 0;
        if (OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS ||
            OH_IPCParcel_ReadInt64(data, &key) != OH_IPC_SUCCESS ||
            OH_IPCParcel_ReadInt64(data, &period) != OH_IPC_SUCCESS) return kPhoneVirglRelayError;
        uint32_t len = sizeof(version) + sizeof(key) + sizeof(period);
        if (!sock::SockWritePod(fd, len) || !sock::SockWritePod(fd, version) ||
            !sock::SockWritePod(fd, key) || !sock::SockWritePod(fd, period)) return kPhoneVirglRelayError;
        break;
    }
    case vi::kShutdownRequest:
    case vi::kQuerySurfacesRequest: {
        int32_t version = 0;
        if (OH_IPCParcel_ReadInt32(data, &version) != OH_IPC_SUCCESS) return kPhoneVirglRelayError;
        uint32_t len = sizeof(version);
        if (!sock::SockWritePod(fd, len) || !sock::SockWritePod(fd, version)) return kPhoneVirglRelayError;
        break;
    }
    default:
        return kPhoneVirglRelayError;
    }

    uint32_t rlen = 0;
    if (!sock::SockReadAll(fd, &rlen, sizeof(rlen)) || rlen == 0 || rlen > 4096)
        return kPhoneVirglRelayError;
    if (code == vi::kQuerySurfacesRequest) {
        std::vector<uint8_t> buf(rlen);
        if (!sock::SockReadAll(fd, buf.data(), rlen)) return kPhoneVirglRelayError;
        if (reply &&
            OH_IPCParcel_WriteBuffer(reply, buf.data(), static_cast<int32_t>(rlen)) != OH_IPC_SUCCESS)
            return kPhoneVirglRelayError;
    } else {
        int32_t result = -1;
        if (rlen != sizeof(result) || !sock::SockReadAll(fd, &result, sizeof(result)))
            return kPhoneVirglRelayError;
        if (reply && OH_IPCParcel_WriteInt32(reply, result) != OH_IPC_SUCCESS)
            return kPhoneVirglRelayError;
    }
    return OH_IPC_SUCCESS;
}
