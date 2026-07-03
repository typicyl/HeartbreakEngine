// Core/Easing.h - tiny easing-curve toolkit for time-driven interpolation (UI
// animation, transitions). Curves map a normalized t in [0,1] to an eased [0,1];
// blend actual values with glm::mix(a, b, Ease(curve, t)).
#pragma once

#include "Core/Types.h"

#include <algorithm>
#include <cmath>

namespace hbe::ease {

enum class Curve : u8 {
    Linear = 0,
    InQuad,
    OutQuad,
    InOutQuad,
    InCubic,
    OutCubic,
    InOutCubic,
    Smoothstep,
};

inline f32 Ease(Curve c, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (c) {
        case Curve::InQuad:     return t * t;
        case Curve::OutQuad:    return t * (2.0f - t);
        case Curve::InOutQuad:  return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        case Curve::InCubic:    return t * t * t;
        case Curve::OutCubic:   { const f32 u = t - 1.0f; return u * u * u + 1.0f; }
        case Curve::InOutCubic: return t < 0.5f ? 4.0f * t * t * t
                                                : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case Curve::Smoothstep: return t * t * (3.0f - 2.0f * t);
        case Curve::Linear:
        default:                return t;
    }
}

} // namespace hbe::ease
