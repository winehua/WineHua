#pragma once
#include <memory>
#include <cstdint>
#include <native_window/external_window.h>

class DesktopCompositor;
namespace winehua::direct {
class VulkanDesktopRenderer;
void SetDirectDesktopVulkanEnabled(bool enabled);
bool DirectDesktopVulkanEnabled();
struct DirectDesktopPerformance {
    bool active = false;
    uint64_t presents = 0, gamePresents = 0;
};
DirectDesktopPerformance GetDirectDesktopPerformance();

// Single XComponent Vulkan owner. Guest images remain in GPU memory;
// Wayland SHM decorations/UI are separate cached scene textures.
class DirectVulkanDesktopCompositor {
public:
    explicit DirectVulkanDesktopCompositor(DesktopCompositor& compositor);
    ~DirectVulkanDesktopCompositor();
    bool Initialize(OHNativeWindow* window);
    bool Render(int width, int height);
    int Width() const;
    int Height() const;
    int ContentWidth() const;
    int ContentHeight() const;
private:
    std::unique_ptr<VulkanDesktopRenderer> renderer_;
};
}
