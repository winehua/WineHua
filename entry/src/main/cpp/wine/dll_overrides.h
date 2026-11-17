#pragma once

#include <string>

namespace winehua {

// Merge individual DLL rules, preserving unrelated backend/runtime rules.
// An empty update explicitly clears the variable. Malformed input leaves
// result untouched and returns false.
// Whether a module has an explicit native-first environment rule.
bool NativeDllPreferred(const std::string& overrides, const std::string& module);

bool MergeDllOverrides(const std::string& baseline, const std::string& update,
                       std::string& result);

}
