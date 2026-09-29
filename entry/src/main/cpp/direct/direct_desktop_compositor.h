#pragma once

#include <EGL/egl.h>
#include <memory>
#include "compositor/frame/geometry.h"

class DesktopCompositor;

namespace winehua::direct {

// Guest Vulkan images are imported directly into the desktop GL context.
// Only the final App composition uses GL; no Vulkan commands or pixels cross
// a software renderer, and no CPU image mapping/upload is involved.
class DirectDesktopCompositor {
public:
    DirectDesktopCompositor(DesktopCompositor& compositor, EGLDisplay display);
    ~DirectDesktopCompositor(); // called with its GL context current
    bool Update();
    void Draw(uint32_t rootId, int desktopW, int desktopH, const FitRect& fit);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace winehua::direct
