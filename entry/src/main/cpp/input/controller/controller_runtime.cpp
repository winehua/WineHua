#include "input/controller/controller_runtime.h"

#include "input/controller/controller_hub.h"
#include "input/controller/gamepad_bridge.h"
#include "input/game_controller_bridge.h"

#include <mutex>

namespace winehua {
namespace controller {
namespace {

std::mutex gModeMutex;
std::string gOutputMode = "dinput";

}  // namespace

void SetOutputMode(const std::string& mode)
{
    std::lock_guard<std::mutex> lock(gModeMutex);
    if (mode == "keyboard_legacy") gOutputMode = "keyboard_legacy";
    else gOutputMode = "dinput";
}

std::string GetOutputMode()
{
    std::lock_guard<std::mutex> lock(gModeMutex);
    return gOutputMode;
}

bool IsDinputMode()
{
    return GetOutputMode() == "dinput";
}

bool EnsureBridgeForWineLaunch(const std::string& runtimeDir)
{
    if (!IsDinputMode()) {
        GamepadBridge::Instance().Stop();
        ControllerHub::Instance().SetEnabled(false);
        return false;
    }
    std::string sock = runtimeDir.empty() ? std::string("/data/storage/el2/base/files/.wine") : runtimeDir;
    if (!sock.empty() && sock.back() != '/') sock += '/';
    sock += "whgp.sock";
    // master has no product GamepadManager lifecycle. Keep the Native Host
    // functional on its own; devices without the Kit degrade to touch input.
    EnsurePhysicalGamepadInitialized();
    ControllerHub::Instance().SetEnabled(true);
    GamepadBridge::Instance().AttachToHub();
    return GamepadBridge::Instance().Start(sock);
}

void AppendWineGamepadEnv(std::vector<std::string>& env)
{
    if (!IsDinputMode()) {
        env.push_back("WINEHUA_CONTROLLER_HUB=0");
        env.push_back("WINEHUA_GAMEPAD_ENABLE=0");
        env.push_back("WINEHUA_GAMEPAD_MODE=keyboard_legacy");
        return;
    }
    const std::string path = GamepadBridge::Instance().SocketPath();
    env.push_back("WINEHUA_CONTROLLER_HUB=1");
    env.push_back("WINEHUA_GAMEPAD_ENABLE=1");
    env.push_back("WINEHUA_GAMEPAD_MODE=dinput");
    if (!path.empty()) env.push_back("WINEHUA_GAMEPAD_SOCKET=" + path);
}

}  // namespace controller
}  // namespace winehua
