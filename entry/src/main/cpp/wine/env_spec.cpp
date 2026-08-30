#include "env_spec.h"

namespace winehua {

bool IsPerProcessFdEnvKey(const std::string& key) {
    return key == "WINESERVERSOCKET" ||
           key == "WINE_OHOS_AUDIO_ENABLE" ||
           key == "WINE_OHOS_AUDIO_BOOTSTRAP_FD" ||
           key == "WINE_OHOS_AUDIO_PROTOCOL_VERSION";
}

bool IsEntryParamsEncodable(const std::string& line) {
    return line.find('|') == std::string::npos &&
           line.find('\n') == std::string::npos;
}

long EnvSpec::indexOf(const std::string& key) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].first == key) return static_cast<long>(i);
    return -1;
}

void EnvSpec::set(const std::string& key, const std::string& value) {
    if (key.empty()) return;
    long i = indexOf(key);
    if (i >= 0) entries_[i].second = value;
    else entries_.emplace_back(key, value);
}

void EnvSpec::setLine(const std::string& line) {
    const size_t sep = line.find('=');
    if (sep == std::string::npos || sep == 0) return;
    set(line.substr(0, sep), line.substr(sep + 1));
}

void EnvSpec::mergeFrom(const EnvSpec& other) {
    for (const auto& kv : other.entries_) set(kv.first, kv.second);
}

bool EnvSpec::has(const std::string& key) const { return indexOf(key) >= 0; }

const std::string* EnvSpec::get(const std::string& key) const {
    const long i = indexOf(key);
    return i >= 0 ? &entries_[i].second : nullptr;
}

std::string EnvSpec::serializeEntryParams() const {
    std::string out;
    for (const auto& kv : entries_) {
        if (IsPerProcessFdEnvKey(kv.first)) continue;
        const std::string line = kv.first + "=" + kv.second;
        if (!IsEntryParamsEncodable(line)) continue;
        out += "|__env=";
        out += line;
    }
    return out;
}

EnvSpec EnvSpec::fromLines(const std::vector<std::string>& lines) {
    EnvSpec spec;
    for (const std::string& line : lines) spec.setLine(line);
    return spec;
}

std::vector<std::string> EnvSpec::toLines() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.first + "=" + kv.second);
    return out;
}

} // namespace winehua
