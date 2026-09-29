#include "native_buffer_socket.h"
#include <IPCKit/ipc_kit.h>
#include <native_buffer/native_buffer.h>
#include <dlfcn.h>
#include <hilog/log.h>
#include <cstring>
#include <sys/stat.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "DIRECT_SHARED_BUFFER"

namespace winehua::direct::shared {
namespace {
struct Parcel {
    OHIPCParcel* value = OH_IPCParcel_Create();
    ~Parcel() { if (value) OH_IPCParcel_Destroy(value); }
};
bool Fail(const char** stage, const char* value) { *stage = value; return false; }
bool ValidDescriptor(const Packet& p) {
    return p.kind == Kind::Buffer && p.width > 0 && p.width <= 8192 && p.height > 0 &&
        p.height <= 8192 && p.stride > 0 && p.allocationSize > 0 && p.hasMainFd == 1 &&
        p.reserveFds < kMaxFds && p.reserveInts <= kMaxInts &&
        p.fdCount == 1 + p.reserveFds &&
        ((p.reserveFds == 0 && p.reserveInts == 0 && p.codecFlags == 0) ||
         (p.reserveFds == 1 && p.reserveInts == 65 && p.codecFlags == kAllocatorLocalPair13 &&
          p.extraInts[13] == 0 && p.extraInts[14] == 0)) &&
        (p.originalFieldCount == 0 || (p.originalFieldCount == 5 &&
         (p.originalFields[0] == 0 || p.originalFields[0] == 1)));
}
// Experimental Mate 80 allocator schema: local ReadFromParcel/re-encode
// changes reserved ints 13/14 as a local 64-bit value. Its meaning is
// undocumented; fd/mapping checks did not establish an address identity.
// Clear both halves on the wire and require system
// registration to regenerate a nonzero value. Every other field and every fd
// must survive an exact roundtrip. Other opaque allocator schemas are refused.
bool CanonicalizeMetadata(Packet& p) {
    if (p.reserveFds == 0 && p.reserveInts == 0) return true;
    if (p.reserveFds != 1 || p.reserveInts != 65 || (p.extraInts[13] == 0 && p.extraInts[14] == 0)) return false;
    p.extraInts[13] = p.extraInts[14] = 0;
    p.codecFlags = kAllocatorLocalPair13;
    return true;
}
bool SameDescriptor(const Message& a, const Message& b) {
    const auto& x = a.packet;
    const auto& y = b.packet;
    if (x.sequence != y.sequence || x.reserveFds != y.reserveFds || x.reserveInts != y.reserveInts ||
        x.width != y.width || x.height != y.height || x.stride != y.stride ||
        x.allocationSize != y.allocationSize || x.format != y.format || x.usage != y.usage ||
        x.physicalMetadata != y.physicalMetadata || x.hasMainFd != y.hasMainFd ||
        x.originalFieldCount != y.originalFieldCount || x.codecFlags != y.codecFlags ||
        a.fds.size() != b.fds.size() ||
        memcmp(x.extraInts, y.extraInts, x.reserveInts * sizeof(int32_t)) ||
        memcmp(x.originalFields, y.originalFields, x.originalFieldCount * sizeof(int32_t))) {
        OH_LOG_ERROR(LOG_APP, "roundtrip: seq=%{public}u/%{public}u reserveFds=%{public}u/%{public}u "
            "reserveInts=%{public}u/%{public}u geometryEqual=%{public}d usageEqual=%{public}d "
            "phyEqual=%{public}d originalCount=%{public}u/%{public}u originalEqual=%{public}d",
            x.sequence, y.sequence, x.reserveFds, y.reserveFds, x.reserveInts, y.reserveInts,
            x.width == y.width && x.height == y.height && x.stride == y.stride &&
                x.allocationSize == y.allocationSize && x.format == y.format,
            x.usage == y.usage, x.physicalMetadata == y.physicalMetadata,
            x.originalFieldCount, y.originalFieldCount,
            memcmp(x.originalFields, y.originalFields, sizeof(x.originalFields)) == 0);
        for (uint32_t i = 0; i < x.reserveInts && i < y.reserveInts; ++i)
            if (x.extraInts[i] != y.extraInts[i])
                OH_LOG_ERROR(LOG_APP, "roundtrip reserveInt changed index=%{public}u value=%{public}d/%{public}d "
                    "mainFd=%{public}d/%{public}d extraFd=%{public}d/%{public}d", i, x.extraInts[i], y.extraInts[i],
                    a.fds.empty() ? -1 : a.fds[0], b.fds.empty() ? -1 : b.fds[0],
                    a.fds.size() < 2 ? -1 : a.fds[1], b.fds.size() < 2 ? -1 : b.fds[1]);
        return false;
    }
    // fd integers belong to each process. Compare the kernel backing identity,
    // not their numeric values, after the system decoder duplicated them.
    for (size_t i = 0; i < a.fds.size(); ++i) {
        struct stat first{}, second{};
        const int firstResult = fstat(a.fds[i], &first);
        const int secondResult = fstat(b.fds[i], &second);
        if (firstResult || secondResult ||
            first.st_dev != second.st_dev || first.st_ino != second.st_ino ||
            first.st_size != second.st_size) {
            OH_LOG_ERROR(LOG_APP, "roundtrip fd mismatch index=%{public}zu stat=%{public}d/%{public}d "
                "devEqual=%{public}d inoEqual=%{public}d sizeEqual=%{public}d",
                i, firstResult, secondResult, first.st_dev == second.st_dev,
                first.st_ino == second.st_ino, first.st_size == second.st_size);
            return false;
        }
    }
    return true;
}
}

bool SameBufferConfig(OH_NativeBuffer* buffer, const Packet& packet) {
    if (!buffer) return false;
    OH_NativeBuffer_Config config{};
    OH_NativeBuffer_GetConfig(buffer, &config);
    return config.width == packet.width && config.height == packet.height &&
        config.stride == packet.stride && config.format == packet.format &&
        static_cast<uint32_t>(config.usage) == static_cast<uint32_t>(packet.usage);
}

bool EncodeBuffer(OH_NativeBuffer* buffer, Message& output, const char** stage) {
    output.CloseFds();
    auto write = reinterpret_cast<int32_t (*)(OH_NativeBuffer*, OHIPCParcel*)>(
        dlsym(RTLD_DEFAULT, "OH_NativeBuffer_WriteToParcel"));
    if (!write) return Fail(stage, "native_buffer_parcel_api");
    Parcel parcel;
    if (!parcel.value || write(buffer, parcel.value) != 0) return Fail(stage, "native_buffer_serialize");
    Packet p{};
    p.kind = Kind::Buffer;
    int32_t hasBuffer = 0;
    auto read32 = [&](auto& item) {
        int32_t value;
        if (OH_IPCParcel_ReadInt32(parcel.value, &value) != OH_IPC_SUCCESS) return false;
        item = value;
        return true;
    };
    auto read64 = [&](uint64_t& item) {
        int64_t value;
        if (OH_IPCParcel_ReadInt64(parcel.value, &value) != OH_IPC_SUCCESS) return false;
        item = static_cast<uint64_t>(value);
        return true;
    };
    // Experimental adapter for the scalar SurfaceBuffer/BufferHandle parcel
    // layout. Validate with GetConfig and a system decode/re-encode roundtrip.
    // SDK BufferHandle's tail does not match the API 26 device ABI, so never
    // dereference its reserve arrays or copy its pointer-bearing struct.
    // No raw Binder object or process pointer is transported.
    if (!read32(p.sequence) || !read32(hasBuffer) || hasBuffer != 1 ||
        !read32(p.reserveFds) || !read32(p.reserveInts) || p.reserveFds >= kMaxFds || p.reserveInts > kMaxInts ||
        !read32(p.width) || !read32(p.stride) || !read32(p.height) || !read32(p.allocationSize) ||
        !read32(p.format) || !read64(p.usage) || !read64(p.physicalMetadata) ||
        !read32(p.hasMainFd) || p.hasMainFd != 1) return Fail(stage, "native_buffer_parcel_layout");
    for (uint32_t i = 0; i < 1 + p.reserveFds; ++i) {
        int32_t fd = -1;
        if (OH_IPCParcel_ReadFileDescriptor(parcel.value, &fd) != OH_IPC_SUCCESS || fd < 0)
            return Fail(stage, "native_buffer_parcel_fd");
        output.fds.push_back(fd); // ReadFileDescriptor duplicates; this Message owns it.
    }
    for (uint32_t i = 0; i < p.reserveInts; ++i)
        if (!read32(p.extraInts[i])) return Fail(stage, "native_buffer_parcel_ints");
    const int tail = OH_IPCParcel_GetReadableBytes(parcel.value);
    if (tail == 20) {
        p.originalFieldCount = 5;
        for (auto& field : p.originalFields)
            if (!read32(field)) return Fail(stage, "native_buffer_original_fields");
    } else if (tail != 0) return Fail(stage, "native_buffer_unknown_parcel_tail");
    p.fdCount = static_cast<uint32_t>(output.fds.size());
    if (!CanonicalizeMetadata(p)) return Fail(stage, "native_buffer_allocator_schema");
    if (!ValidDescriptor(p) || OH_IPCParcel_GetReadableBytes(parcel.value) != 0)
        return Fail(stage, "native_buffer_descriptor");
    if (!SameBufferConfig(buffer, p)) {
        OH_NativeBuffer_Config config{};
        OH_NativeBuffer_GetConfig(buffer, &config);
        OH_LOG_ERROR(LOG_APP, "parcel/config: w=%{public}d/%{public}d h=%{public}d/%{public}d "
            "stride=%{public}d/%{public}d format=%{public}d/%{public}d usage=%{public}u/%{public}u",
            p.width, config.width, p.height, config.height, p.stride, config.stride, p.format, config.format,
            static_cast<uint32_t>(p.usage), static_cast<uint32_t>(config.usage));
        return Fail(stage, "native_buffer_config_mismatch");
    }
    output.packet = p;
    *stage = "buffer_encoded";
    return true;
}

bool DecodeBuffer(const Message& input, OH_NativeBuffer** buffer, const char** stage,
                  CodecProgress progress, void* context) {
    *buffer = nullptr;
    const auto& p = input.packet;
    if (!ValidDescriptor(p) || input.fds.size() != p.fdCount) return Fail(stage, "native_buffer_wire_descriptor");
    auto read = reinterpret_cast<int32_t (*)(OHIPCParcel*, OH_NativeBuffer**)>(
        dlsym(RTLD_DEFAULT, "OH_NativeBuffer_ReadFromParcel"));
    if (!read) return Fail(stage, "native_buffer_parcel_api");
    Parcel parcel;
    if (!parcel.value) return Fail(stage, "native_buffer_local_parcel");
    auto write32 = [&](int32_t value) { return OH_IPCParcel_WriteInt32(parcel.value, value) == OH_IPC_SUCCESS; };
    auto write64 = [&](uint64_t value) { return OH_IPCParcel_WriteInt64(parcel.value, static_cast<int64_t>(value)) == OH_IPC_SUCCESS; };
    if (!write32(p.sequence) || !write32(1) || !write32(p.reserveFds) || !write32(p.reserveInts) ||
        !write32(p.width) || !write32(p.stride) || !write32(p.height) || !write32(p.allocationSize) ||
        !write32(p.format) || !write64(p.usage) || !write64(p.physicalMetadata) || !write32(p.hasMainFd))
        return Fail(stage, "native_buffer_rebuild_fields");
    for (int fd : input.fds)
        if (OH_IPCParcel_WriteFileDescriptor(parcel.value, fd) != OH_IPC_SUCCESS)
            return Fail(stage, "native_buffer_rebuild_fds");
    for (uint32_t i = 0; i < p.reserveInts; ++i)
        if (!write32(p.extraInts[i])) return Fail(stage, "native_buffer_rebuild_ints");
    for (uint32_t i = 0; i < p.originalFieldCount; ++i)
        if (!write32(p.originalFields[i])) return Fail(stage, "native_buffer_rebuild_tail");
    if (progress) progress("codec_system_read_from_parcel", context);
    if (read(parcel.value, buffer) != 0 || !*buffer) return Fail(stage, "native_buffer_deserialize");
    if (progress) progress("codec_system_read_complete", context);
    if (!SameBufferConfig(*buffer, p) || OH_IPCParcel_GetReadableBytes(parcel.value) != 0) {
        OH_NativeBuffer_Unreference(*buffer);
        *buffer = nullptr;
        return Fail(stage, "native_buffer_imported_config");
    }
    Message encoded;
    if (progress) progress("codec_system_reencode", context);
    if (!EncodeBuffer(*buffer, encoded, stage) || !SameDescriptor(input, encoded)) {
        OH_NativeBuffer_Unreference(*buffer);
        *buffer = nullptr;
        return Fail(stage, "native_buffer_roundtrip_descriptor");
    }
    *stage = "buffer_decoded";
    return true;
}
} // namespace winehua::direct::shared
