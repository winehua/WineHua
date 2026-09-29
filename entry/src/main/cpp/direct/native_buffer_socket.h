#pragma once

#include <cstdint>
#include <vector>

struct OH_NativeBuffer;

namespace winehua::direct::shared {

constexpr uint32_t kMagic = 0x57485342; // WHSB
constexpr uint32_t kVersion = 5;
constexpr uint32_t kMaxFds = 16;
constexpr uint32_t kMaxInts = 256;
constexpr char kSocketName[] = "direct_shared_buffer";
constexpr char kDiagnosticName[] = "direct_shared_log";
static_assert(sizeof(kSocketName) <= 21 && sizeof(kDiagnosticName) <= 21, "fork bootstrap fd names");
constexpr uint32_t kFrameCount = 4;
constexpr uint32_t kAllocatorLocalPair13 = 1;
enum class Kind : uint32_t { Buffer = 1, Ready, Render, Frame, Release, Finish, Complete, Stage, Error, Allocate };

// Versioned scalar data only. Vulkan handles and process addresses never cross.
struct Packet {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t size = sizeof(Packet);
    Kind kind = Kind::Error;
    uint32_t fdCount = 0;
    int32_t pid = 0;
    int32_t result = 0;
    uint32_t frame = 0;
    uint32_t imports = 0;
    uint32_t allocations = 0;
    uint32_t exports = 0;
    uint32_t fenceImports = 0;
    uint32_t fenceExports = 0;
    uint32_t realFenceImports = 0;
    uint32_t realFenceExports = 0;
    uint32_t sequence = 0;
    uint32_t reserveFds = 0;
    uint32_t reserveInts = 0;
    int32_t width = 0;
    int32_t stride = 0;
    int32_t height = 0;
    int32_t allocationSize = 0;
    int32_t format = 0;
    uint64_t usage = 0;
    uint64_t physicalMetadata = 0;
    uint32_t hasMainFd = 0;
    uint32_t originalFieldCount = 0;
    uint32_t codecFlags = 0;
    int32_t originalFields[5]{};
    int32_t extraInts[kMaxInts]{};
    char stage[64]{};
    char loaderPath[128]{};
    char deviceName[128]{};
};

struct Message {
    Packet packet{};
    std::vector<int> fds;
    Message() = default;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    ~Message();
    void CloseFds();
};

bool Send(int socket, Packet packet, const std::vector<int>& fds = {});
bool Receive(int socket, Message& message, int timeoutMs = 15000);
bool EncodeBuffer(OH_NativeBuffer* buffer, Message& message, const char** stage);
using CodecProgress = void (*)(const char*, void*);
bool DecodeBuffer(const Message& message, OH_NativeBuffer** buffer, const char** stage,
                  CodecProgress progress = nullptr, void* context = nullptr);
bool SameBufferConfig(OH_NativeBuffer* buffer, const Packet& packet);

} // namespace winehua::direct::shared
