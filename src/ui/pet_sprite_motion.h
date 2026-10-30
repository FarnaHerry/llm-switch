#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace llmswitch::ui {

// Fractions of the sprite viewport; the retained HUI controller supplies one-shot progress.
struct PetSpriteReaction {
    float x = 0.0F;
    float y = 0.0F;
    float scale = 1.0F;
    float sparkle = 0.0F;
};

inline PetSpriteReaction SamplePetSpriteReaction(int kind, float progress) {
    const float p = std::clamp(progress, 0.0F, 1.0F);
    if (p <= 0.0F || p >= 1.0F) return {};
    const float arc = std::sin(p * std::numbers::pi_v<float>);
    if (kind == 0) return {.y = -0.085F * arc, .scale = 1.0F + 0.025F * arc};
    if (kind == 1) {
        return {.x = 0.05F * std::sin(p * 6.0F * std::numbers::pi_v<float>) * (1.0F - p)};
    }
    return {.scale = 1.0F + 0.04F * arc, .sparkle = arc};
}

} // namespace llmswitch::ui
