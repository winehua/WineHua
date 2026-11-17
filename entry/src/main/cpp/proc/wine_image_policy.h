#pragma once
#include "spawn_codec.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>

namespace winehua::spawn {

struct WineImagePolicy {
    bool pe32 = false;
    bool dynamicBase = false;
    bool relocationsStripped = false;
    uint32_t imageBase = 0;
    uint32_t imageSize = 0;

    bool PreservePreferredBase() const {
        return pe32 && imageBase >= 0x10000 && imageSize &&
            uint64_t(imageBase) + imageSize <= 0x100000000ULL &&
            (imageBase & 0xffff) == 0;
    }
};

inline uint16_t ImageRead16(const unsigned char* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t ImageRead32(const unsigned char* p) {
    return uint32_t(ImageRead16(p)) | uint32_t(ImageRead16(p + 2)) << 16;
}

// Read bounded headers only; never map a game into the ArkTS/broker process.
inline WineImagePolicy ReadWineImagePolicy(const std::string& path) {
    WineImagePolicy image;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return image;
    unsigned char dos[64], nt[24 + 224];
    if (fread(dos, 1, sizeof(dos), file) == sizeof(dos) &&
        ImageRead16(dos) == 0x5a4d) {
        const uint32_t offset = ImageRead32(dos + 60);
        if (offset >= sizeof(dos) && offset <= 1024 * 1024 &&
            !fseek(file, offset, SEEK_SET) &&
            fread(nt, 1, sizeof(nt), file) == sizeof(nt) &&
            ImageRead32(nt) == 0x4550 && ImageRead16(nt + 4) == 0x14c &&
            ImageRead16(nt + 20) >= 224 && ImageRead16(nt + 24) == 0x10b &&
            (ImageRead16(nt + 22) & 0x0002) && !(ImageRead16(nt + 22) & 0x2000)) {
            image.pe32 = true;
            image.dynamicBase = (ImageRead16(nt + 24 + 70) & 0x40) != 0;
            image.relocationsStripped = (ImageRead16(nt + 22) & 1) != 0;
            image.imageBase = ImageRead32(nt + 24 + 28);
            image.imageSize = ImageRead32(nt + 24 + 56);
        }
    }
    fclose(file);
    return image;
}

inline std::string ImageEnvironment(const Request& request, const char* key) {
    const std::string prefix = std::string(key) + '=';
    for (auto it = request.env.rbegin(); it != request.env.rend(); ++it)
        if (it->rfind(prefix, 0) == 0) return it->substr(prefix.size());
    return {};
}

inline std::string WineImageNativePath(const Request& request) {
    std::string path = ProcessPath(request);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);
    if (path.rfind("\\??\\", 0) == 0 || path.rfind("\\\\?\\", 0) == 0)
        path.erase(0, 4);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.size() >= 3 && path[1] == ':' && path[2] == '/') {
        char drive = path[0];
        if (drive >= 'A' && drive <= 'Z') drive += 'a' - 'A';
        if (drive == 'z') return request.home + "/" + path.substr(3);
        if (drive == 'c') {
            const auto prefix = ImageEnvironment(request, "WINEPREFIX");
            if (!prefix.empty()) return prefix + "/drive_c/" + path.substr(3);
        }
        return {};
    }
    if (!path.empty() && path[0] == '/') return path;
    return {}; // Wine resolves bare builtin names; no guessed system directory.
}

inline bool ImageRangeOccupied(const WineImagePolicy& image, FILE* maps) {
    if (!maps) return true; // Unknown occupancy requires a clean address space for a valid PE32 range.
    char line[1024];
    unsigned long long begin, end;
    const uint64_t imageEnd = uint64_t(image.imageBase) + image.imageSize;
    while (fgets(line, sizeof(line), maps))
        if (sscanf(line, "%llx-%llx", &begin, &end) == 2 &&
            begin < imageEnd && end > image.imageBase) return true;
    return ferror(maps) != 0;
}

inline bool RequiresFreshWineProcess(const Request& request) {
    const auto image = ReadWineImagePolicy(WineImageNativePath(request));
    if (!image.PreservePreferredBase()) return false;
    // Create/IPC children retain ArkTS low-address mappings. Start children
    // provide a fresh native address space. DYNAMIC_BASE permits guest ASLR;
    // it does not make unrelated native heap reservations part of that contract.
    FILE* maps = fopen("/proc/self/maps", "r");
    const bool occupied = ImageRangeOccupied(image, maps);
    if (maps) fclose(maps);
    return occupied;
}

} // namespace winehua::spawn
