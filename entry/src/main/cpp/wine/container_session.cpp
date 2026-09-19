#include "container_session.h"

#include "wine_constants.h"

#include <cctype>
#include <mutex>

namespace winehua {
namespace {

std::mutex gContainerMutex;
ContainerSession gActiveContainer = {
    kDefaultContainerId,
    WINE_PREFIX,
    WINE_PREFIX "/wine-wayland",
};

bool IsContainerCharacter(char ch)
{
    const unsigned char value = static_cast<unsigned char>(ch);
    return std::isalnum(value) || ch == '_' || ch == '-';
}

} // namespace

bool IsValidContainerId(const std::string& id)
{
    if (id.empty() || id.size() > 64) return false;
    if (!std::isalnum(static_cast<unsigned char>(id.front()))) return false;
    for (char ch : id)
        if (!IsContainerCharacter(ch)) return false;
    return true;
}

bool ResolveContainerSession(const std::string& id, ContainerSession* out)
{
    if (!out || !IsValidContainerId(id)) return false;

    out->id = id;
    if (id == kDefaultContainerId)
    {
        out->prefixDir = WINE_PREFIX;
    }
    else
    {
        out->prefixDir = std::string(WINE_FILES_DIR) + "/containers/" + id + "/prefix";
    }
    out->waylandSocket = out->prefixDir + "/wine-wayland";
    return true;
}

bool SetActiveContainerSession(const std::string& id, ContainerSession* out)
{
    ContainerSession resolved;
    if (!ResolveContainerSession(id, &resolved)) return false;

    std::lock_guard<std::mutex> lock(gContainerMutex);
    gActiveContainer = resolved;
    if (out) *out = resolved;
    return true;
}

ContainerSession GetActiveContainerSession()
{
    std::lock_guard<std::mutex> lock(gContainerMutex);
    return gActiveContainer;
}

bool IsActiveContainerSession(const std::string& id)
{
    std::lock_guard<std::mutex> lock(gContainerMutex);
    return gActiveContainer.id == id;
}

} // namespace winehua
