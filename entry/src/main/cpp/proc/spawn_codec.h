#pragma once
#include "spawn_protocol.h"
#include <string>
#include <vector>
#include <algorithm>

namespace winehua::spawn {
struct Request {
    std::string home, bin;
    std::vector<std::string> argv, env, names;
};
inline std::vector<const char*> Pointers(const std::vector<std::string>& values) {
    std::vector<const char*> result;
    for (const auto& value : values) result.push_back(value.c_str());
    return result;
}
inline bool Encode(const Request& r, std::vector<unsigned char>& bytes) {
    auto argv = Pointers(r.argv), env = Pointers(r.env), names = Pointers(r.names);
    for (const auto* values : {&r.argv, &r.env, &r.names})
        for (const auto& value : *values) if (value.find('\0') != std::string::npos) return false;
    if (r.home.find('\0') != std::string::npos || r.bin.find('\0') != std::string::npos ||
        argv.size() > VP_ITEMS_CAP || env.size() > VP_ITEMS_CAP || names.size() > VP_FDS_CAP) return false;
    vp_spawn_spec spec{r.home.c_str(), r.bin.c_str(), argv.data(), env.data(), names.data(),
        static_cast<uint32_t>(argv.size()), static_cast<uint32_t>(env.size()), static_cast<uint32_t>(names.size())};
    unsigned char* encoded = nullptr; size_t size = 0;
    if (vp_encode(&spec, &encoded, &size)) return false;
    bytes.assign(encoded, encoded + size); free(encoded); return true;
}
inline void Copy(const vp_spawn_spec& s, Request& r) {
    r = {}; r.home = s.home; r.bin = s.bin;
    for (uint32_t i = 0; i < s.argc; ++i) r.argv.emplace_back(s.argv[i]);
    for (uint32_t i = 0; i < s.envc; ++i) r.env.emplace_back(s.env[i]);
    for (uint32_t i = 0; i < s.fdc; ++i) r.names.emplace_back(s.names[i]);
}
inline bool Decode(const std::vector<unsigned char>& bytes, Request& r) {
    vp_spawn_spec s{};
    if (vp_decode(bytes.data(), bytes.size(), &s)) return false;
    Copy(s, r); vp_free_spec(&s); return true;
}
inline bool EncodeEntry(const Request& r, std::string& output) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    if (!Encode(r, bytes) || 5 + ((bytes.size() + 2) / 3) * 4 >= VP_ENTRY_CAP) return false;
    output = VP_ENTRY_PREFIX;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t v = static_cast<uint32_t>(bytes[i]) << 16 |
            (i + 1 < bytes.size() ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        output += alphabet[(v >> 18) & 63]; output += alphabet[(v >> 12) & 63];
        output += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        output += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return true;
}
inline bool DecodeEntry(const char* entry, Request& r) {
    if (!entry || strnlen(entry, VP_ENTRY_CAP) >= VP_ENTRY_CAP || strncmp(entry, VP_ENTRY_PREFIX, 5)) return false;
    const std::string encoded(entry + 5);
    if (encoded.empty() || encoded.size() % 4) return false;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    for (size_t i = 0; i < encoded.size(); i += 4) {
        uint32_t value = 0; int padding = 0;
        for (size_t j = 0; j < 4; ++j) {
            const char ch = encoded[i + j];
            if (ch == '=') { if (i + 4 != encoded.size() || j < 2) return false; ++padding; value <<= 6; }
            else {
                const size_t at = alphabet.find(ch);
                if (at == std::string::npos || padding) return false;
                value = (value << 6) | static_cast<uint32_t>(at);
            }
        }
        if (padding > 2 || (padding == 1 && (value & 255)) || (padding == 2 && (value & 65535))) return false;
        bytes.push_back(value >> 16);
        if (padding < 2) bytes.push_back(value >> 8);
        if (!padding) bytes.push_back(value);
        if (bytes.size() > VP_PAYLOAD_CAP) return false;
    }
    return Decode(bytes, r);
}
inline bool EnvFlag(const Request& r, const char* key, bool fallback = false) {
    const std::string prefix = std::string(key) + '=';
    for (const auto& env : r.env) {
        if (env == prefix + '1') fallback = true;
        else if (env == prefix + '0') fallback = false;
    }
    return fallback;
}
inline bool MatchNames(const Request& r, std::vector<std::string> actual) {
    auto expected = r.names;
    std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
    return expected == actual;
}
inline std::string ProcessPath(const Request& r) {
    for (const auto& arg : r.argv)
        if (!arg.empty() && arg != "wine" && arg != "__winehua_desktop__") return arg;
    return "wine";
}
} // namespace winehua::spawn
