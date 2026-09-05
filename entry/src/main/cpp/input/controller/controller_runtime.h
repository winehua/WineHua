#pragma once

#include <string>
#include <vector>

namespace winehua {
namespace controller {

// "dinput" (default) | "keyboard_legacy"
void SetOutputMode(const std::string& mode);
std::string GetOutputMode();
bool IsDinputMode();

// Called from wine launch path: start WHGP + enable hub when dinput.
bool EnsureBridgeForWineLaunch(const std::string& runtimeDir);
void AppendWineGamepadEnv(std::vector<std::string>& env);

}  // namespace controller
}  // namespace winehua
