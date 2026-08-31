#include "compositor_utils.h"
#include <unistd.h>

uint64_t MakeSurfaceKey(uint32_t clientPid, uint32_t surfaceId)
{
    return (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
}

uint32_t GetWaylandClientPid(wl_client* client)
{
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
    return pid > 0 ? static_cast<uint32_t>(pid) : 0;
}
