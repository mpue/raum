#pragma once

// raum's look in one place: a holographic HUD -- deep space behind, light drawn
// in lines. Cyan is raum's own colour; each room adds its accent on top.

#include <cmath>

#include <glm/glm.hpp>

namespace raum::theme {

inline constexpr glm::vec3 kHolo{0.24f, 0.90f, 1.00f};      // lines, highlights
inline constexpr glm::vec3 kHoloDeep{0.05f, 0.42f, 0.55f};  // fills behind the lines
inline constexpr glm::vec3 kText{0.86f, 0.97f, 1.00f};
inline constexpr glm::vec3 kDim{0.46f, 0.66f, 0.74f};
inline constexpr glm::vec3 kPanel{0.012f, 0.035f, 0.055f};  // the glass of a panel
inline constexpr glm::vec3 kWarn{1.00f, 0.71f, 0.28f};
inline constexpr glm::vec3 kSpaceZenith{0.006f, 0.012f, 0.026f};
inline constexpr glm::vec3 kSpaceHorizon{0.020f, 0.055f, 0.085f};

// Smooth steps for animations, all on 0..1.
inline float easeOut(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
inline float easeInOut(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
// A little past the target and back: things that "snap" into place.
inline float easeBack(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float c = 1.70158f;
    const float u = t - 1.0f;
    return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
}

} // namespace raum::theme
