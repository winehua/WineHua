#pragma once

#include <string>

namespace winehua {

std::string ToLower(std::string value);
std::string TrimCopy(const std::string& value);
void ReplaceAll(std::string& value, const std::string& needle, const std::string& replacement);
std::string TrimQuotes(const char* value);

} // namespace winehua
