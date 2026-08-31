#pragma once

#include <string>

namespace winehua {

bool EnsureDir(const std::string& path);
bool FileExists(const std::string& path);
bool DirExists(const std::string& path);
bool DirHasSharedObjectWithPrefix(const std::string& dir, const std::string& prefix);
std::string DirNameCopy(const std::string& path);
const char* BasenameOfPath(const char* path);
void RemoveDir(const char* path);
std::string CurrentSharedObjectDir();

} // namespace winehua
