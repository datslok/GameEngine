#include "render/gpu/level_fade.h"
#include "core/dither.h"

#include <algorithm>

std::uint32_t drawListFade(std::uint32_t entry) {
    return (entry & drawListFadeMask) >> drawListFadeShift;
}

// Steps of 1 / 63 of the band, held to the last step at its end.
std::uint32_t levelFadeField(float share, bool coarse) {
    const std::uint32_t step = std::min(static_cast<std::uint32_t>(std::max(share, 0.0f) * static_cast<float>(drawListFadeSteps)), drawListFadeSteps - 1u);
    return (coarse ? drawListFadeCoarse : 0u) | (step + 1u);
}

float levelFadeThreshold(std::uint32_t field) {
    return (static_cast<float>((field & ~drawListFadeCoarse) - 1u) + 0.5f) / static_cast<float>(drawListFadeSteps);
}

bool levelFadeKeeps(std::uint32_t field, float noise) {
    if (field == 0u) {
        return true;
    }

    const bool below = noise < levelFadeThreshold(field);
    return (field & drawListFadeCoarse) != 0u ? below : !below;
}

// Offset from the sun cascades' fixed dither (scene_surface.glsl's 23, 41), and by the frame as the fog turns its own.
float levelFadeNoise(float pixelX, float pixelY, std::uint32_t frame) {
    const float turn = 5.588238f * static_cast<float>(frame);
    return interleavedGradientNoise(pixelX + 47.0f + turn, pixelY + 17.0f + turn);
}
