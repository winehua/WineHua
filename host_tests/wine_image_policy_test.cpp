#include "proc/wine_image_policy.h"
#include <cassert>
#include <unistd.h>
#include <iostream>

using namespace winehua::spawn;

static std::vector<unsigned char> Image(bool dynamicBase = false) {
    std::vector<unsigned char> bytes(512);
    bytes[0] = 'M'; bytes[1] = 'Z'; bytes[60] = 64;
    auto p = bytes.data() + 64;
    p[0] = 'P'; p[1] = 'E'; p[4] = 0x4c; p[5] = 1;
    p[20] = 224; p[22] = 2;
    p[24] = 0x0b; p[25] = 1;
    p[24 + 30] = 0x40; // preferred ImageBase = 0x00400000
    p[24 + 58] = 1;    // SizeOfImage = 0x00010000
    p[24 + 70] = dynamicBase ? 0x40 : 0;
    return bytes;
}

static bool Occupied(const WineImagePolicy& image, const char* contents) {
    FILE* maps = tmpfile(); assert(maps);
    fputs(contents, maps); rewind(maps);
    const bool result = ImageRangeOccupied(image, maps);
    fclose(maps); return result;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            auto image = ReadWineImagePolicy(argv[i]);
            std::cout << argv[i] << " pe32=" << image.pe32
                      << " dynamicBase=" << image.dynamicBase
                      << " stripped=" << image.relocationsStripped
                      << " base=0x" << std::hex << image.imageBase << std::dec
                      << " preservePreferred=" << image.PreservePreferredBase() << '\n';
        }
        return 0;
    }
    char name[] = "/tmp/vp-image-policy-XXXXXX";
    int fd = mkstemp(name); assert(fd >= 0); close(fd);
    auto write = [&](const std::vector<unsigned char>& bytes) {
        FILE* f = fopen(name, "wb"); assert(f);
        assert(fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()); fclose(f);
    };
    write(Image());
    auto image = ReadWineImagePolicy(name);
    assert(image.pe32 && image.PreservePreferredBase());
    assert(image.imageBase == 0x400000 && image.imageSize == 0x10000);
    assert(Occupied(image, "00140000-20040000 rw-p 00000000 00:00 0\n"));
    assert(!Occupied(image, "00100000-00400000 rw-p\n00410000-20040000 rw-p\n"));
    assert(Occupied(image, "00400000-00410000 ---p\n"));
    assert(Occupied(image, "00400001-00400002 rw-p\n"));
    assert(ImageRangeOccupied(image, nullptr));
    write(Image(true));
    assert(ReadWineImagePolicy(name).PreservePreferredBase());
    auto bytes = Image(true); bytes[64 + 22] |= 1; write(bytes);
    assert(ReadWineImagePolicy(name).PreservePreferredBase());
    bytes = Image(); bytes[64 + 5] = 0x86; write(bytes); // x64 stays Direct-eligible
    assert(!ReadWineImagePolicy(name).pe32);
    bytes = Image(); bytes[64 + 23] = 0x20; write(bytes); // DLL, not executable
    assert(!ReadWineImagePolicy(name).pe32);
    bytes = Image(); bytes[64 + 24] = 0x0b; bytes[64 + 25] = 2; write(bytes);
    assert(!ReadWineImagePolicy(name).pe32);
    bytes = Image(); bytes[64 + 20] = 72; write(bytes);
    assert(!ReadWineImagePolicy(name).pe32);
    bytes = Image(); bytes.resize(100); write(bytes);
    assert(!ReadWineImagePolicy(name).pe32);
    bytes = Image(); bytes[60] = 0xff; bytes[63] = 0x7f; write(bytes);
    assert(!ReadWineImagePolicy(name).pe32);
    image.imageBase = 0xffff0000; image.imageSize = 0x20000;
    assert(!image.PreservePreferredBase());
    Request request; request.home = "/session/home";
    request.argv = {"wine", "Z:\\games\\Old Game\\game.exe"};
    assert(WineImageNativePath(request) == "/session/home/games/Old Game/game.exe");
    request.argv = {"__winehua_desktop__", "wine", "\"\\??\\C:\\test.exe\""};
    request.env = {"WINEPREFIX=/old", "WINEPREFIX=/active"};
    assert(WineImageNativePath(request) == "/active/drive_c/test.exe");
    request.argv = {"C:/test.exe"}; request.env.clear();
    assert(WineImageNativePath(request).empty());
    request.argv = {"D:/test.exe"}; assert(WineImageNativePath(request).empty());
    request.argv = {"wine", "cmd.exe"}; assert(WineImageNativePath(request).empty());
    request.argv = {name}; write(Image());
    // The test executable occupies 0x00400000 under -no-pie: real maps + real PE header.
    assert(RequiresFreshWineProcess(request));
    write(Image(true)); assert(RequiresFreshWineProcess(request));
    unlink(name);
    std::cout << "PASS: preferred PE32 base, mapped ranges, DYNAMIC_BASE/native conflicts/x64, path resolution and malformed headers\n";
}
