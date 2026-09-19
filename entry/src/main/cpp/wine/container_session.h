#ifndef WINEHUA_CONTAINER_SESSION_H
#define WINEHUA_CONTAINER_SESSION_H

#include <string>

namespace winehua {

// The default container preserves the historical single-prefix location. New
// game containers are always derived from a constrained identifier.
constexpr const char kDefaultContainerId[] = "default";

struct ContainerSession {
    std::string id;
    std::string prefixDir;
    std::string waylandSocket;
};

bool IsValidContainerId(const std::string& id);
bool ResolveContainerSession(const std::string& id, ContainerSession* out);

// A WineHua process has one broker and one wineserver session at a time. The
// caller must drain the previous session before changing this value.
bool SetActiveContainerSession(const std::string& id, ContainerSession* out = nullptr);
ContainerSession GetActiveContainerSession();
bool IsActiveContainerSession(const std::string& id);

} // namespace winehua

#endif // WINEHUA_CONTAINER_SESSION_H
