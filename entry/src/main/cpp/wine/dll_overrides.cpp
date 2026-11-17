#include "dll_overrides.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace winehua {
namespace {

std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

std::string Lower(std::string value)
{
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}

using Rules = std::vector<std::pair<std::string, std::string>>;

bool Parse(const std::string& value, Rules& rules)
{
    if (value.find_first_of("\r\n") != std::string::npos) return false;
    for (size_t begin = 0; begin < value.size();) {
        size_t end = value.find(';', begin);
        if (end == std::string::npos) end = value.size();
        const std::string clause = Trim(value.substr(begin, end - begin));
        begin = end + 1;
        if (clause.empty()) continue;
        const size_t equal = clause.find('=');
        if (equal == std::string::npos || clause.find('=', equal + 1) != std::string::npos)
            return false;
        const std::string order = Lower(Trim(clause.substr(equal + 1)));
        std::string normalizedOrder;
        for (size_t pos = 0; pos < order.size();) {
            size_t next = order.find_first_of(", \t", pos);
            if (next == std::string::npos) next = order.size();
            const std::string token = order.substr(pos, next - pos);
            pos = next + 1;
            if (token.empty()) continue;
            if (token != "n" && token != "native" && token != "b" && token != "builtin" &&
                token != "disabled") return false;
            if (token == "disabled") {
                if (order != "disabled") return false;
                break;
            }
            const char mode = token[0];
            if (normalizedOrder.find(mode) == std::string::npos) {
                if (!normalizedOrder.empty()) normalizedOrder += ',';
                normalizedOrder += mode;
            }
        }
        const std::string modules = Trim(clause.substr(0, equal));
        if (modules.empty() || modules.front() == ',' || modules.back() == ',') return false;
        for (size_t pos = 0; pos <= modules.size();) {
            size_t next = modules.find_first_of(", \t", pos);
            if (next == std::string::npos) next = modules.size();
            std::string name = Lower(Trim(modules.substr(pos, next - pos)));
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
                name.resize(name.size() - 4);
            if (name.empty()) {
                if (next == modules.size()) break;
                pos = next + 1;
                continue;
            }
            auto existing = std::find_if(rules.begin(), rules.end(), [&](const auto& rule) {
                return rule.first == name;
            });
            if (existing == rules.end()) rules.emplace_back(name, normalizedOrder);
            else existing->second = normalizedOrder;
            if (next == modules.size()) break;
            pos = next + 1;
        }
    }
    return true;
}

}

bool NativeDllPreferred(const std::string& overrides, const std::string& module)
{
    Rules rules;
    if (!Parse(overrides, rules)) return false;
    const std::string name = Lower(module);
    // Wine resolves an unqualified explicit name before its wildcard form.
    for (const std::string& candidate : {name, "*" + name})
        for (const auto& rule : rules)
            if (rule.first == candidate)
                return !rule.second.empty() && rule.second[0] == 'n';
    return false;
}

bool MergeDllOverrides(const std::string& baseline, const std::string& update,
                       std::string& result)
{
    if (update.empty()) { result.clear(); return true; }
    Rules rules;
    if (!Parse(baseline, rules) || !Parse(update, rules)) return false;
    std::string merged;
    for (const auto& rule : rules) {
        if (!merged.empty()) merged += ';';
        merged += rule.first + '=' + rule.second;
    }
    result = std::move(merged);
    return true;
}

}
