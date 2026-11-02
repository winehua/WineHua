#pragma once

#include "input/controller/controller_types.h"

#include <algorithm>
#include <cmath>

namespace winehua {
namespace controller {

struct CanonicalHat {
    int8_t x = 0;
    int8_t y = 0;
};

inline float ClampUnit(float v, float lo, float hi)
{
    if (!std::isfinite(v)) return 0.f;
    return std::clamp(v, lo, hi);
}

inline int8_t SignWithThreshold(double raw, double threshold = 0.5)
{
    if (!std::isfinite(raw)) return 0;
    if (raw < -threshold) return -1;
    if (raw > threshold) return 1;
    return 0;
}

// OH Game Controller Kit thumbs match SDL raw polarity: up is negative.
// Convert to Canonical Controller Space (+Y = Up).
inline Stick2D NormalizeOhosThumb(double rawX, double rawY)
{
    Stick2D out;
    out.x = ClampUnit(static_cast<float>(rawX), -1.f, 1.f);
    out.y = ClampUnit(static_cast<float>(-rawY), -1.f, 1.f);
    return out;
}

// OH hat +Y is Down. Canonical hat +Y is Up.
inline CanonicalHat NormalizeOhosHat(double rawX, double rawY)
{
    CanonicalHat out;
    out.x = SignWithThreshold(rawX);
    out.y = static_cast<int8_t>(-SignWithThreshold(rawY));
    return out;
}

inline float NormalizeOhosTrigger(double rawValue)
{
    return ClampUnit(static_cast<float>(rawValue), 0.f, 1.f);
}

// Overlay screen space: +Y is down. Canonical stick +Y is Up.
inline Stick2D NormalizeScreenThumb(float dx, float dy, float radius)
{
    Stick2D out;
    if (!(radius > 0.f) || !std::isfinite(radius) || !std::isfinite(dx) || !std::isfinite(dy)) {
        return out;
    }
    out.x = ClampUnit(dx / radius, -1.f, 1.f);
    out.y = ClampUnit(-dy / radius, -1.f, 1.f);
    return out;
}

}  // namespace controller
}  // namespace winehua
