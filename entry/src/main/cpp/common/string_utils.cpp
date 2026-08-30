#include "string_utils.h"

#include <cctype>

namespace winehua {

std::string ToLower(std::string value)
{
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string TrimCopy(const std::string& value)
{
    size_t start = 0;
    size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

void ReplaceAll(std::string& value, const std::string& needle, const std::string& replacement)
{
    size_t pos = 0;

    if (needle.empty()) return;

    while ((pos = value.find(needle, pos)) != std::string::npos)
    {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

std::string TrimQuotes(const char* value)
{
    std::string s = value ? value : "";
    while (!s.empty() && (s.front() == '"' || s.front() == '\'')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == '\'')) s.pop_back();
    return s;
}

} // namespace winehua
