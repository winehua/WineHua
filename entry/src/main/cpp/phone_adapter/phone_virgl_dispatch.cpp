// phone_virgl_dispatch.cpp — 手机适配层：virgl IPC dispatch（child 侧）
// 手机 fork 路径下没有 Binder 驱动回调 OnVirglIpcRequest；
// 本线程从配置 socket 读请求、按 virgl_ipc 协议构造 parcel、调用 handler，回送 reply。
// 线协议与 phone_virgl_relay.cpp 的 PhoneVirgl_RelayRequest 严格对应。
#include "phone_virgl_dispatch.h"
#include "phone_socket.h"
#include "graphics/virgl_ipc_protocol.h"

#include <cstdint>
#include <string>
#include <thread>

#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#include <hilog/log.h>

namespace {

constexpr uint32_t kMaxPayload = 4096;

void DispatchLoop(int fd, PhoneVirglHandler handler) {
    namespace vi = winehua::virgl_ipc;
    namespace sock = phone_adapter;

    OH_LOG_INFO(LOG_APP, "[PhoneVirgl] dispatch loop start fd=%{public}d", fd);

    while (true) {
        uint32_t code = 0, len = 0;
        if (!sock::SockReadPod(fd, code) || !sock::SockReadPod(fd, len) || len > kMaxPayload) break;

        OHIPCParcel* data = OH_IPCParcel_Create();
        OHIPCParcel* reply = OH_IPCParcel_Create();
        bool ok = (data && reply);

        if (ok) {
            switch (code) {
            case vi::kConfigureRequest: {
                int32_t version = 0;
                std::string s[vi::kHostConfigStringCount];
                ok = sock::SockReadPod(fd, version);
                for (uint32_t i = 0; ok && i < vi::kHostConfigStringCount; ++i)
                    ok = sock::SockReadStr(fd, s[i]);
                if (ok) {
                    OH_IPCParcel_WriteInt32(data, version);
                    for (uint32_t i = 0; i < vi::kHostConfigStringCount; ++i)
                        OH_IPCParcel_WriteString(data, s[i].c_str());
                }
                break;
            }
            case vi::kDetachSurfaceRequest: {
                int32_t version = 0; int64_t key = 0;
                ok = sock::SockReadPod(fd, version) && sock::SockReadPod(fd, key);
                if (ok) {
                    OH_IPCParcel_WriteInt32(data, version);
                    OH_IPCParcel_WriteInt64(data, key);
                }
                break;
            }
            case vi::kSetFramePeriodRequest: {
                int32_t version = 0; int64_t key = 0, period = 0;
                ok = sock::SockReadPod(fd, version) && sock::SockReadPod(fd, key) && sock::SockReadPod(fd, period);
                if (ok) {
                    OH_IPCParcel_WriteInt32(data, version);
                    OH_IPCParcel_WriteInt64(data, key);
                    OH_IPCParcel_WriteInt64(data, period);
                }
                break;
            }
            case vi::kShutdownRequest:
            case vi::kQuerySurfacesRequest: {
                int32_t version = 0;
                ok = sock::SockReadPod(fd, version);
                if (ok) OH_IPCParcel_WriteInt32(data, version);
                break;
            }
            default:
                ok = false;
            }
        }

        if (!ok) {
            if (data) OH_IPCParcel_Destroy(data);
            if (reply) OH_IPCParcel_Destroy(reply);
            break;
        }

        handler(code, data, reply, nullptr);

        if (code == vi::kQuerySurfacesRequest) {
            const uint8_t* bytes = OH_IPCParcel_ReadBuffer(
                reply, static_cast<int32_t>(sizeof(vi::SurfaceQueryReply)));
            uint32_t rlen = bytes ? static_cast<uint32_t>(sizeof(vi::SurfaceQueryReply)) : 0;
            sock::SockWritePod(fd, rlen);
            if (rlen) sock::SockWriteAll(fd, bytes, rlen);
        } else {
            int32_t result = -1;
            OH_IPCParcel_ReadInt32(reply, &result);
            uint32_t rlen = sizeof(result);
            sock::SockWritePod(fd, rlen);
            sock::SockWritePod(fd, result);
        }

        OH_IPCParcel_Destroy(data);
        OH_IPCParcel_Destroy(reply);
    }

    OH_LOG_INFO(LOG_APP, "[PhoneVirgl] dispatch loop exit fd=%{public}d", fd);
}

} // namespace

void PhoneVirgl_DispatchStart(int fd, PhoneVirglHandler handler) {
    std::thread(DispatchLoop, fd, handler).detach();
}
