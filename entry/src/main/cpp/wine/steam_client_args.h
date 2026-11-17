#pragma once
#include <cstddef>
#include <cstring>

namespace winehua {

// Steam forwards every argument after -applaunch to the game.
inline int SteamClientArgCount(int argc, char* const argv[])
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] && std::strcmp(argv[i], "-applaunch") == 0) return i;
    return argc;
}

inline bool HasSteamClientArg(int argc, char* const argv[], const char* flag)
{
    const int end = SteamClientArgCount(argc, argv);
    for (int i = 1; i < end; ++i)
        if (argv[i] && std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

inline bool InsertSteamClientArg(int& argc, char* argv[], std::size_t capacity, char* flag)
{
    if (argc < 0 || static_cast<std::size_t>(argc) + 1 >= capacity) return false;
    const int at = SteamClientArgCount(argc, argv);
    for (int i = argc; i > at; --i) argv[i] = argv[i - 1];
    argv[at] = flag;
    argv[++argc] = nullptr;
    return true;
}

} // namespace winehua
