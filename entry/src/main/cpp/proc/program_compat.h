#pragma once
#include <string>

namespace winehua {

inline std::string ProcessImageName(int argc, char* const argv[]) {
    auto name = [](const char* value) {
        std::string image = value ? value : "";
        if (image.size() >= 2 && image.front() == '"' && image.back() == '"')
            image = image.substr(1, image.size() - 2);
        const auto slash = image.find_last_of("/\\");
        if (slash != std::string::npos) image.erase(0, slash + 1);
        for (char& c : image) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return image;
    };
    if (argc <= 0 || !argv || !argv[0]) return {};
    auto image = name(argv[0]);
    if (image == "wine" || image == "wine64")
        image = argc > 1 ? name(argv[1]) : "";
    return image;
}

} // namespace winehua
